// A Java test package for component_mle (stands for a Maven jar). Build:
// javac --release 21 -d classes src/demo/Stats.java && jar cf mle-demo-1.0.0.jar -C classes .
package demo;

public final class Stats {
  private Stats() {}

  public static double median(double... v) {
    double[] s = v.clone();
    java.util.Arrays.sort(s);
    return s.length % 2 == 1 ? s[s.length / 2] : (s[s.length / 2 - 1] + s[s.length / 2]) / 2;
  }
}
