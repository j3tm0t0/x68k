// XEiJ 用 Java エージェント (ヘッドレスな動作確認用)。
// コマンドファイル (既定 build/agent.cmd) に 1 行 1 コマンドを書くと実行して削除する。
//   screenshot              xeij.Keyboard.kbdDoCapture()  -> capture/N.png
//   textcopy                xeij.TextCopy.txcCopy()       -> クリップボード (pbpaste で取得)
//   call <class> <method>   引数なし public static メソッドを呼ぶ
//   set <class> <field> <int>  public static int フィールドに値を代入する
//   get <class> <field>        public static フィールドの値を標準出力に表示する
//   format <path.hds> <MB>     XEiJ 内蔵のフォーマッタで Human68k 入り SCSI HDS を作る (参照用)
//   dump <file> <addr> <len>   メインメモリ (xeij.MainMemory.mmrM8) の一部をファイルに書く (16 進可: 0x...)
// -Dpicosim=host:port があれば SCSI ID / Z のメールボックスを harness/picosim につなぐ (PicoBridge.java)
import java.io.File;
import java.lang.instrument.Instrumentation;
import java.nio.file.Files;

public class XEiJAgent {
  public static void premain(String args, Instrumentation inst) {
    xeij.PicoBridge.start ();
    final File cmdFile = new File(args == null || args.isEmpty() ? "build/agent.cmd" : args);
    Thread t = new Thread(() -> {
      while (true) {
        try {
          Thread.sleep(300);
          if (!cmdFile.exists()) continue;
          java.util.List<String> lines = Files.readAllLines(cmdFile.toPath());
          cmdFile.delete();
          for (String line : lines) {
            String[] a = line.trim().split("\\s+");
            if (a.length == 0 || a[0].isEmpty()) continue;
            String cls, mth;
            if (a[0].equals("screenshot")) { cls = "xeij.Keyboard"; mth = "kbdDoCapture"; }
            else if (a[0].equals("textcopy")) { cls = "xeij.TextCopy"; mth = "txcCopy"; }
            else if (a[0].equals("call") && a.length == 3) { cls = a[1]; mth = a[2]; }
            else if (a[0].equals("format") && a.length == 3) {
              Class<?> spc = Class.forName("xeij.SPC");
              spc.getField("spcFormatPartitionMegaBytes").setInt(null, Integer.parseInt(a[2]));
              spc.getField("spcFormatBytesPerRecord").setInt(null, 512);
              spc.getField("spcFormatPartitioningOn").setBoolean(null, true);
              spc.getField("spcFormatCopyHumanSysOn").setBoolean(null, true);
              spc.getField("spcFormatCopyCommandXOn").setBoolean(null, true);
              Object r = spc.getMethod("spcFormatFiles", java.io.File[].class)
                  .invoke(null, (Object) new java.io.File[] { new java.io.File(a[1]) });
              System.out.println("agent: format " + a[1] + " -> " + r);
              continue;
            }
            else if (a[0].equals("dump") && a.length == 4) {
              byte[] m = (byte[]) Class.forName("xeij.MainMemory").getField("mmrM8").get(null);
              int addr = Integer.decode(a[2]), len = Integer.decode(a[3]);
              Files.write(new File(a[1]).toPath(), java.util.Arrays.copyOfRange(m, addr, addr + len));
              System.out.println("agent: dump " + a[1] + " " + a[2] + " " + a[3]);
              continue;
            }
            else if (a[0].equals("get") && a.length == 3) {
              Object v = Class.forName(a[1]).getField(a[2]).get(null);
              if (v != null && v.getClass().isArray()) v = java.util.Arrays.deepToString((Object[]) v);
              System.out.println("agent: get " + a[1] + "." + a[2] + "=" + v);
              continue;
            }
            else if (a[0].equals("set") && a.length == 4) {
              Class.forName(a[1]).getField(a[2]).setInt(null, Integer.parseInt(a[3]));
              System.out.println("agent: set " + a[1] + "." + a[2] + "=" + a[3]);
              continue;
            }
            else { System.out.println("agent: unknown command " + line); continue; }
            Class<?> k = Class.forName(cls);
            javax.swing.SwingUtilities.invokeAndWait(() -> {
              try { k.getMethod(mth).invoke(null); System.out.println("agent: " + cls + "." + mth + " done"); }
              catch (Throwable e) { e.printStackTrace(); }
            });
          }
        } catch (Throwable e) { e.printStackTrace(); }
      }
    }, "xeij-agent");
    t.setDaemon(true);
    t.start();
  }
}
