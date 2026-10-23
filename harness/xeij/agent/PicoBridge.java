// XEiJ の SCSI ID を外のプロセス (harness/picosim の Pico の代わり) につなぐ橋 (XEiJAgent から起動する)。
//
// X68000 Z の Pico W は Z から見ると SCSI のディスク (ID0 = 起動 HDS、ID6 = netpipe など) で、Z の上のプログラムは
// 2 通りの道でそれを読み書きする:
//   1. IOCS _SCSIDRV (XEiJ では SPC MB89352 の模倣と XEiJ の SCSI ROM)  → PicoUnit (SPC.SCUnit の子) が受ける
//   2. Z の Pseudo SCSI BIOS のメールボックス 0xEA0060 を直に (npd.x の zio())  → このクラスの郵便受けスレッドが受ける
// どちらも 1 命令 = 1 往復で、TCP (既定 127.0.0.1:6868) の picosim に CDB とデータを渡し、状態とデータを受け取る。
// 形式は harness/picosim/README.md。値はすべてビッグエンディアン。
//
//   要求 32 + n バイト: "PSQ1" seq(4) id(1) path(1: 0=SPC, 1=メールボックス) cdblen(1) 0(1) out_len(4) cdb(16) data(out_len)
//   応答 16 + n バイト: "PSA1" seq(4) status(1: SCSI の状態。0=GOOD 2=CHECK CONDITION) key(1) asc(1) ascq(1) in_len(4) data(in_len)
//
// 設定はシステムプロパティ (run.sh の XEIJ_JAVA_OPTS で渡す):
//   -Dpicosim=127.0.0.1:6868   接続先。無ければ何もしない
//   -Dpicosim.ids=6            SPC で picosim に渡す ID (例 "0,6")。XEiJ の -si<n> の HDS の代わりになる
//   -Dpicosim.mailbox=on       メールボックス 0xEA0060 を模倣する (off で切る)
//   -Dpicosim.log=1            命令ごとに標準出力に出す
//   -Dpicosim.timeout=5000     応答を待つ ms。過ぎたら切ってつなぎ直す (だめなら NOT READY)。止まった picosim で
//                              SPC の道 (MPU のスレッド) が固まらないように
//
// XEiJ には手を入れない。SPC.spcUnitArray[id] を PicoUnit に差し替え (spcReset は配列の中身の scuReset を呼ぶので
// リセット後も残る)、0xEA0000-0xEA0FFF のページを主記憶 (MMD_MMR、mmrM8 のその番地) にする (spcReset が拡張 SCSI の
// 有無で毎回 MMD_NUL に戻すので、郵便受けスレッドが戻し直す)。
package xeij;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.locks.LockSupport;

public class PicoBridge {

  static final int MB = 0x00ea0060;           //Z の Pseudo SCSI BIOS のメールボックス
  static final int MB_PAGE = 0x00ea0000;
  static final int MB_PAGE_END = 0x00ea1000;

  static String host;
  static int port;
  static boolean log;
  static int timeout;
  static Socket sock;
  static DataInputStream in;
  static DataOutputStream out;
  static int seq;
  static long lastFail;                       //つながらなかった時刻 (1 秒は再接続しない)
  //計数 (agent の get xeij.PicoBridge stCmds などで読む)
  public static long stCmds, stMailbox, stFails, stNanos;

  static final VarHandle BA = MethodHandles.arrayElementVarHandle (byte[].class);

  //応答
  static final class Reply {
    int status, key, asc, ascq;
    byte[] data;
  }

  public static void start () {
    String p = System.getProperty ("picosim");
    if (p == null || p.isEmpty ()) {
      return;
    }
    int c = p.lastIndexOf (':');
    host = c < 0 ? "127.0.0.1" : p.substring (0, c);
    port = Integer.parseInt (c < 0 ? p : p.substring (c + 1));
    log = !"0".equals (System.getProperty ("picosim.log", "0"));
    timeout = Integer.parseInt (System.getProperty ("picosim.timeout", "5000"));
    String ids = System.getProperty ("picosim.ids", "6");
    boolean mailbox = !"off".equals (System.getProperty ("picosim.mailbox", "on"));
    Thread t = new Thread (() -> {
      //SPC.spcInit (XEiJ.main の前半) が済むまで待つ。フレームは spcInit のあと、MPU を起こす前に作られる。
      //ここで SPC を触ると SPC の static 初期化を XEiJ より先に走らせてしまうので、フレームで判断する
      while (java.awt.Frame.getFrames ().length == 0) {
        LockSupport.parkNanos (1_000_000L);
      }
      while (SPC.spcUnitArray == null || SPC.spcUnitArray[15] == null) {
        LockSupport.parkNanos (1_000_000L);
      }
      for (String s : ids.split (",")) {
        if (s.isBlank ()) {
          continue;
        }
        int id = Integer.parseInt (s.trim ());
        PicoUnit u = new PicoUnit (id);
        u.scuReset (SPC.spcSCSIINChip);
        SPC.spcUnitArray[id] = u;
        System.out.println ("picosim: SCSI ID " + id + " -> " + host + ":" + port);
      }
      if (mailbox) {
        System.out.println ("picosim: mailbox 0x" + Integer.toHexString (MB) + " -> " + host + ":" + port);
        mailboxLoop ();
      }
    }, "picosim-bridge");
    t.setDaemon (true);
    t.start ();
  }

  //1 命令を picosim に渡す。つながらなければ null (呼び出し側が NOT READY にする)
  static synchronized Reply transact (int id, int path, byte[] cdb, int cdblen, byte[] data, int off, int len) {
    long t0 = System.nanoTime ();
    for (int attempt = 0; attempt < 2; attempt++) {
      if (sock == null) {
        if (System.currentTimeMillis () - lastFail < 1000) {
          break;
        }
        try {
          Socket s = new Socket ();
          s.setTcpNoDelay (true);
          s.connect (new InetSocketAddress (host, port), 1000);
          s.setSoTimeout (timeout);
          sock = s;
          in = new DataInputStream (new BufferedInputStream (s.getInputStream (), 65536));
          out = new DataOutputStream (new BufferedOutputStream (s.getOutputStream (), 65536));
          System.out.println ("picosim: connected");
        } catch (IOException e) {
          lastFail = System.currentTimeMillis ();
          break;
        }
      }
      try {
        int q = ++seq;
        out.writeInt (0x50535131);  //"PSQ1"
        out.writeInt (q);
        out.writeByte (id);
        out.writeByte (path);
        out.writeByte (cdblen);
        out.writeByte (0);
        out.writeInt (len);
        out.write (cdb, 0, 16);
        if (len > 0) {
          out.write (data, off, len);
        }
        out.flush ();
        if (in.readInt () != 0x50534131 || in.readInt () != q) {  //"PSA1"
          throw new IOException ("bad reply");
        }
        Reply r = new Reply ();
        r.status = in.readUnsignedByte ();
        r.key = in.readUnsignedByte ();
        r.asc = in.readUnsignedByte ();
        r.ascq = in.readUnsignedByte ();
        int n = in.readInt ();
        if (n < 0 || n > 16 << 20) {
          throw new IOException ("bad length " + n);
        }
        r.data = new byte[n];
        in.readFully (r.data);
        stCmds++;
        stNanos += System.nanoTime () - t0;
        if (log) {
          System.out.printf ("picosim: id%d %s %02x lba=%d out=%d -> st=%d key=%d in=%d%n", id, path == 0 ? "spc" : "mbx",
                             cdb[0] & 255, cdblen >= 10 ? (cdb[2] & 255) << 24 | (cdb[3] & 255) << 16 | (cdb[4] & 255) << 8 | cdb[5] & 255 :
                             (cdb[1] & 31) << 16 | (cdb[2] & 255) << 8 | cdb[3] & 255, len, r.status, r.key, n);
        }
        return r;
      } catch (IOException e) {
        System.out.println ("picosim: " + e);
        stFails++;
        try {
          sock.close ();
        } catch (IOException e2) {
        }
        sock = null;
      }
    }
    return null;
  }

  //メールボックス: +0 bit7 使用中, +1 段階 (4 で開始), +2 ID, +3 SCSI 命令, +4 LBA, +8 バイト数, +12 バッファ, +16/+18 状態。
  //Z の上のプログラムは +0 = 0x80, +1 = 4 と書いて +0 の bit7 が落ちるまで待ち、+16/+18 を読んで +1 を 0 に戻す。
  //XEiJ の MPU スレッドとは別のスレッドで受けるので、Z と同じく MPU は待ちの間も動き続ける (npd.x は _CHANGE_PR で譲る)
  static void mailboxLoop () {
    byte[] m = MainMemory.mmrM8;
    long idle = 0;
    for (;;) {
      if (XEiJ.busSuperMap[MB_PAGE >>> XEiJ.BUS_PAGE_BITS] != MemoryMappedDevice.MMD_MMR) {
        //起動時とリセットのたびに SPC.spcReset が MMD_NUL (または拡張 SCSI) に戻す
        XEiJ.busSuper (MemoryMappedDevice.MMD_MMR, MB_PAGE, MB_PAGE_END);
        for (int i = MB_PAGE; i < MB_PAGE_END; i++) {
          m[i] = 0;
        }
      }
      if (((byte) BA.getAcquire (m, MB) & 0x80) == 0 || (byte) BA.getAcquire (m, MB + 1) != 4) {
        //空いているときは軽く待つ (命令が来ていた直後は細かく見る)
        LockSupport.parkNanos (++idle < 2000 ? 20_000L : 200_000L);
        continue;
      }
      idle = 0;
      int id = m[MB + 2] & 7;
      int op = m[MB + 3] & 255;
      int lba = rl (m, MB + 4);
      int bytes = rl (m, MB + 8);
      int buf = rl (m, MB + 12) & XEiJ.BUS_MOTHER_MASK;
      int status = 0xff;
      if ((op == 0x28 || op == 0x2a) && bytes >= 0 && bytes <= 0x100000 && (bytes & 511) == 0 &&
          buf + bytes <= m.length) {
        byte[] cdb = new byte[16];
        cdb[0] = (byte) op;
        cdb[2] = (byte) (lba >>> 24);
        cdb[3] = (byte) (lba >>> 16);
        cdb[4] = (byte) (lba >>> 8);
        cdb[5] = (byte) lba;
        cdb[7] = (byte) (bytes >>> 17);
        cdb[8] = (byte) (bytes >>> 9);
        Reply r = op == 0x2a ? transact (id, 1, cdb, 10, m, buf, bytes) : transact (id, 1, cdb, 10, null, 0, 0);
        if (r != null) {
          status = r.status;
          if (op == 0x28 && status == 0) {
            System.arraycopy (r.data, 0, m, buf, Math.min (r.data.length, bytes));
          }
        }
      } else {
        System.out.printf ("picosim: mailbox op %02x bytes %d buf %08x not supported%n", op, bytes, buf);
      }
      stMailbox++;
      //状態: +16 = SCSI の状態 (0 = GOOD)、+18 = 0。picosim につながらないときは +16 = 0xff
      m[MB + 16] = 0;
      m[MB + 17] = (byte) status;
      m[MB + 18] = 0;
      m[MB + 19] = 0;
      VarHandle.releaseFence ();  //データと状態を書いてから使用中を落とす
      BA.setRelease (m, MB, (byte) (m[MB] & 0x7f));
    }
  }

  static int rl (byte[] m, int a) {
    return (m[a] & 255) << 24 | (m[a + 1] & 255) << 16 | (m[a + 2] & 255) << 8 | m[a + 3] & 255;
  }

  //SPC (IOCS _SCSIDRV) の道。命令を丸ごと picosim に渡す (INQUIRY / READ CAPACITY / REQUEST SENSE / NOT READY も picosim が決める)
  public static class PicoUnit extends SPC.SCUnit {

    byte[] pending;                           //データアウトを待っている命令の CDB
    int pendingLen;

    public PicoUnit (int number) {
      super (number, null);
      abuConnected = true;
      abuInserted = true;
    }

    @Override public void scuCommand (int oc) {
      byte[] cdb = new byte[16];
      int cdblen = SPC.SPC_CDB_LENGTH[oc >> 4];
      System.arraycopy (scuChip.spiCommandBuffer, 0, cdb, 0, Math.min (cdblen, 16));
      int out = dataOutLength (cdb);
      if (out > 0) {
        //データを受け取ってから scuWriteImage で picosim に渡す (count = 1 なので 1 回だけ呼ばれる)。
        //SPC の模倣はデータアウトのあと必ず GOOD にするので、書き込みの失敗は Z 側に伝わらない
        pending = cdb;
        pendingLen = cdblen;
        scuChip.spiDataOutPhase (new byte[out], 0, out, 1);
        return;
      }
      Reply r = transact (abuNumber, 0, cdb, cdblen, null, 0, 0);
      finish (r);
    }

    @Override public void scuWriteImage () {
      byte[] cdb = pending;
      pending = null;
      if (cdb == null) {
        return;
      }
      Reply r = transact (abuNumber, 0, cdb, pendingLen, scuChip.spiWriteHandle, 0, scuChip.spiBufferLimit);
      if (r == null || r.status != 0) {
        System.out.printf ("picosim: id%d write %02x failed (status %s), reported as GOOD by the SPC model%n",
                           abuNumber, cdb[0] & 255, r == null ? "none" : Integer.toString (r.status));
      }
    }

    @Override public void scuReadImage () {
      //データインは 1 回で全部渡すので呼ばれない
    }

    void finish (Reply r) {
      if (r == null) {
        //picosim につながらない: NOT READY (REQUEST SENSE だけは手元で答える)
        if ((scuChip.spiCommandBuffer[0] & 255) == 0x03) {
          byte[] s = new byte[18];
          s[0] = 0x70;
          s[2] = 0x02;
          s[7] = 10;
          s[12] = 0x04;
          s[13] = 0x01;
          int n = Math.min (scuChip.spiCommandBuffer[4] & 255, 18);
          scuChip.spiDataInPhase (s, 0, n == 0 ? 4 : n, 0);
          return;
        }
        scuChip.spiStatusPhase (SPC.SPC_CHECK_CONDITION, SPC.SPC_COMMAND_COMPLETE);
        return;
      }
      if (r.status != 0) {
        scuChip.spiStatusPhase (r.status, SPC.SPC_COMMAND_COMPLETE);
      } else if (r.data.length > 0) {
        scuChip.spiDataInPhase (r.data, 0, r.data.length, 0);
      } else {
        scuChip.spiStatusPhase (SPC.SPC_GOOD, SPC.SPC_COMMAND_COMPLETE);
      }
    }

    //イニシエータから受け取るバイト数 (ブロック長は 512 固定)
    static int dataOutLength (byte[] c) {
      switch (c[0] & 255) {
      case 0x0a:  //Write(6)
        return ((c[4] & 255) == 0 ? 256 : c[4] & 255) * 512;
      case 0x2a:  //Write(10)
      case 0x2e:  //Write and Verify(10)
        return ((c[7] & 255) << 8 | c[8] & 255) * 512;
      case 0xaa:  //Write(12)
        return ((c[6] & 255) << 24 | (c[7] & 255) << 16 | (c[8] & 255) << 8 | c[9] & 255) * 512;
      case 0x15:  //Mode Select(6)
        return c[4] & 255;
      default:
        return 0;
      }
    }
  }
}
