import cf_golden.Aggregator;
import cf_golden.Record;
import cf_golden.RecordParser;
import java.util.List;
import java.util.Map;

public final class PipelineTest {
  private static int tests = 0;
  private static int passed = 0;

  private static void check(boolean condition, String name) {
    tests++;
    if (condition) {
      passed++;
    } else {
      System.err.println("FAILED: " + name);
    }
  }

  public static void main(String[] args) {
    Record parsed = RecordParser.parse("  alpha=7 ").orElseThrow();
    check(parsed.key().equals("alpha") && parsed.value() == 7, "parses trimmed record");
    check(RecordParser.parse("broken").isEmpty(), "rejects malformed line");
    check(RecordParser.parse("=3").isEmpty(), "rejects empty key");

    Map<String, Integer> totals = Aggregator.sum(List.of(
        "alpha=7", "beta=2", "alpha=5", "ignored", "beta=3", "=1"));
    check(totals.equals(Map.of("alpha", 12, "beta", 5)), "aggregates valid lines");
    check(totals.size() == 2, "ignores malformed lines");
    boolean immutable = false;
    try {
      totals.put("gamma", 1);
    } catch (UnsupportedOperationException expected) {
      immutable = true;
    }
    check(immutable, "result map is immutable");

    System.out.println("{\"tests\":" + tests + ",\"passed\":" + passed + "}");
    if (passed != tests) {
      System.exit(1);
    }
  }
}
