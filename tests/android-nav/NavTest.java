package ro.cobrabm.fexdroid;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;

/** Host-JDK test of the route finder (NavData, NavGate): loads an area's .bnv and gate table from an unpacked map pack, runs the
 *  Python prototype's lamp pairs (tools/atlas/nav/javaref.py output) and compares gate states, snapped nodes and route costs
 *  with it; prints the Java timings.  Usage: NavTest PACK_DIR(atlas/maps/nav) AREA REF.json  (exit 1 on a mismatch). */
public final class NavTest {
    static int failures;
    static void check(boolean ok,String what) { if (!ok) { ++failures; System.out.println("FAIL " + what); } }

    public static void main(String[] args) throws Exception {
        File dir = new File(args[0]); String area = args[1];
        long t0 = System.nanoTime();
        byte[] file = Files.readAllBytes(new File(dir, area + ".bnv").toPath());
        NavGate[] gates = NavGate.parse(new String(Files.readAllBytes(new File(dir, area + ".gates.json").toPath()), StandardCharsets.UTF_8));
        long t1 = System.nanoTime();
        NavData d = NavData.load(area, file);
        long t2 = System.nanoTime();
        System.out.printf("%s: %d nodes (%d virtual), %d arcs, %d gates, ~%.1f MB; file %.2f MB, load %.0f ms (gates %.0f ms)%n", area, d.gridCount, d.count - d.gridCount,
            d.adj.length, d.gateCount, d.bytes / 1048576.0, file.length / 1048576.0, (t2 - t1) / 1e6, (t1 - t0) / 1e6);
        check(d.gateCount == gates.length, "gate table has " + gates.length + " gates, graph " + d.gateCount);

        @SuppressWarnings("unchecked")
        Map<String, Object> ref = (Map<String, Object>) MiniJson.parse(new String(Files.readAllBytes(new File(args[2]).toPath()), StandardCharsets.UTF_8));
        check(((Number) ref.get("nodes")).intValue() == d.count, "node count " + d.count + " vs " + ref.get("nodes"));
        @SuppressWarnings("unchecked") Map<String, Object> scenarios = (Map<String, Object>) ref.get("scenarios");
        double worst = 0, javaTotal = 0, pyTotal = 0; long expTotal = 0; int routes = 0; double maxMs = 0;
        for (Map.Entry<String, Object> sc : scenarios.entrySet()) {
            @SuppressWarnings("unchecked") Map<String, Object> s = (Map<String, Object>) sc.getValue();
            NavGate.Flags flags = sc.getKey().equals("open") ? id -> 1 : id -> -1;
            byte[] states = NavGate.states(gates, flags, sc.getKey().equals("keyretry"));
            List<?> want = (List<?>) s.get("states");
            boolean same = want.size() == states.length;
            for (int i = 0; same && i < states.length; ++i) same = ((Number) want.get(i)).intValue() == states[i];
            check(same, sc.getKey() + ": gate states differ from the prototype's live scenario");
            for (Object o : (List<?>) s.get("pairs")) {
                Map<?, ?> p = (Map<?, ?>) o;
                float[] a = pos(p.get("ap")), b = pos(p.get("bp"));
                int[] src = d.snap(a[0], a[1], a[2], 3f, 1), dst = d.snap(b[0], b[1], b[2], 3f, 1);
                if (src.length == 0) src = d.snap(a[0], a[1], a[2], 6f, 1);
                if (dst.length == 0) dst = d.snap(b[0], b[1], b[2], 6f, 1);
                int ps = ((Number) p.get("src")).intValue(), pd = ((Number) p.get("dst")).intValue();
                check(src.length == 1 && src[0] == ps, "source node " + (src.length > 0 ? src[0] : -1) + " vs " + ps);
                check(dst.length == 1 && dst[0] == pd, "destination node " + (dst.length > 0 ? dst[0] : -1) + " vs " + pd);
                if (src.length == 0 || dst.length == 0) continue;
                d.route(src[0], dst[0], states, 400000, 300_000_000L, 1f); // warm up
                NavData.Path path = d.route(src[0], dst[0], states, 400000, 300_000_000L, 1f);
                Object exact = p.get("exact");
                String label = sc.getKey() + " " + p.get("a") + " -> " + p.get("b");
                if (exact == null) { check(path.status != NavData.OK, label + ": Java found a route the prototype did not"); System.out.printf("%-60s no route (Java %d expansions)%n", label, path.expansions); continue; }
                check(path.status == NavData.OK, label + ": Java found no route (status " + path.status + ")");
                if (path.status != NavData.OK) continue;
                double e = ((Number) exact).doubleValue();
                double rel = Math.abs(path.cost - e) / e;
                worst = Math.max(worst, rel);
                check(rel < 0.01, String.format("%s: cost %.2f vs exact %.2f", label, path.cost, e));
                NavData.Route r = d.toRoute(path, states);
                check(r.n >= 2 && r.length <= path.cost * 1.02f + 1f, String.format("%s: smoothed route %.1f m, path cost %.1f", label, r.length, path.cost));
                double ms = path.nanos / 1e6; javaTotal += ms; expTotal += path.expansions; ++routes; maxMs = Math.max(maxMs, ms);
                double pyMs = ((Number) p.get("py_ms")).doubleValue(); pyTotal += pyMs;
                StringBuilder marks = new StringBuilder();
                for (int i = 0; i < r.marks; ++i) marks.append(" ").append("DLUGV".charAt(Math.min(4, Math.max(0, r.mkind[i] == NavData.K_DROP ? 0 : r.mkind[i] == NavData.K_LADDER ? 1 : r.mkind[i] == NavData.K_LIFT ? 2 : r.mkind[i] == NavData.M_GATE ? 3 : 4))));
                System.out.printf("%-60s exact %8.2f  java %8.2f (%+.3f%%)  %6d exp  %6.1f ms   python A* %6.1f ms  route %d pts %.1f m marks[%s ]%n", label, e, path.cost, 100 * (path.cost - e) / e,
                    path.expansions, ms, pyMs, r.n, r.length, marks);
            }
        }
        if (routes > 0) System.out.printf("%d routes: Java %.1f ms mean (max %.1f), %.0f expansions mean, %.2f us/expansion; Python A* %.1f ms mean; worst cost difference %.3f%%%n",
            routes, javaTotal / routes, maxMs, (double) expTotal / routes, javaTotal * 1000 / expTotal, pyTotal / routes, worst * 100);
        System.out.println(failures == 0 ? "OK" : failures + " failure(s)");
        System.exit(failures == 0 ? 0 : 1);
    }
    static float[] pos(Object o) { List<?> l = (List<?>) o; return new float[]{((Number) l.get(0)).floatValue(), ((Number) l.get(1)).floatValue(), ((Number) l.get(2)).floatValue()}; }
}
