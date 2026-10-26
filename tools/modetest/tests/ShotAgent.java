// XEiJ 用の最小エージェント。制御ファイル (引数) に書いた命令を実行して消す。
//   shot <file.png>   XEiJ が合成した画面 (xeij.XEiJ.pnlBM、1024 画素/行) を PNG に書く
// この環境では XEiJ の kbdDoCapture が黒い画像しか出さないので、画面の BufferedImage を直接書く。
import java.io.File;
import java.lang.instrument.Instrumentation;
import java.nio.file.Files;

public class ShotAgent {
  public static void premain(String args, Instrumentation inst) {
    final File cmd = new File(args);
    Thread t = new Thread(() -> {
      while (true) {
        try {
          Thread.sleep(200);
          if (!cmd.exists()) continue;
          java.util.List<String> lines = Files.readAllLines(cmd.toPath());
          cmd.delete();
          for (String line : lines) {
            String[] a = line.trim().split("\\s+");
            if (a[0].equals("info")) {
              Class<?> c = Class.forName("xeij.XEiJ");
              for (String f : new String[] {"pnlScreenImageLeftArray", "pnlBMLeftArray", "pnlBM", "pnlBMLeft", "pnlScreenImageLeft"}) {
                Object v = c.getField(f).get(null);
                System.out.println("agent: " + f + " = " + (v == null ? "null" : v.getClass().isArray() ? "array " + java.lang.reflect.Array.getLength(v) : v));
              }
              for (String f : new String[] {"pnlScreenWidth", "pnlScreenHeight", "PNL_BM_WIDTH", "PNL_BM_HEIGHT", "PNL_BM_OFFSET_BITS", "pnlBMRead", "pnlBMWrite"})
                System.out.println("agent: " + f + " = " + c.getField(f).get(null));
            }
            if (a[0].equals("dump") && a.length == 4) {
              byte[] m = (byte[]) Class.forName("xeij.MainMemory").getField("mmrM8").get(null);
              int addr = Integer.decode(a[2]), len = Integer.decode(a[3]);
              Files.write(new File(a[1]).toPath(), java.util.Arrays.copyOfRange(m, addr, addr + len));
            }
            if (a[0].equals("shot") && a.length == 2) {
              Class<?> c = Class.forName("xeij.XEiJ");
              int[] bm = (int[]) c.getField("pnlBM").get(null);
              int w = c.getField("pnlScreenWidth").getInt(null), h = c.getField("pnlScreenHeight").getInt(null);
              int stride = c.getField("PNL_BM_WIDTH").getInt(null);
              java.awt.image.BufferedImage im = new java.awt.image.BufferedImage(w, h, java.awt.image.BufferedImage.TYPE_INT_RGB);
              for (int y = 0; y < h; y++) im.setRGB(0, y, w, 1, bm, y * stride, w);
              boolean ok = javax.imageio.ImageIO.write(im, "png", new File(a[1]));
              System.out.println("agent: shot " + a[1] + " " + im.getWidth() + "x" + im.getHeight() + " " + ok);
            }
          }
        } catch (Throwable e) { e.printStackTrace(); }
      }
    });
    t.setDaemon(true);
    t.start();
  }
}
