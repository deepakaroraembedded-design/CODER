import cf_golden.Ledger;
import cf_golden.LineItem;
import cf_golden.Money;
import cf_golden.OrderProcessor;
import java.util.List;

public final class OrderTest {
  private static int tests = 0;
  private static int passed = 0;

  private static void check(boolean condition, String name) {
    tests++;
    if (condition) passed++;
    else System.err.println("FAILED: " + name);
  }

  public static void main(String[] args) {
    Ledger empty = OrderProcessor.process(List.of());
    check(empty.itemCount() == 0 && empty.total().cents() == 0, "empty order");

    Ledger ledger = OrderProcessor.process(List.of(
        new LineItem("A", 2, new Money(500)),
        new LineItem("B", 1, new Money(250)),
        new LineItem("A", 1, new Money(100))));
    check(ledger.itemCount() == 3, "duplicate SKUs stay distinct");
    check(ledger.total().cents() == 1350, "total");
    check(ledger.items().size() == 3, "items exposed");

    boolean immutable = false;
    try {
      ledger.items().add(new LineItem("C", 1, new Money(1)));
    } catch (UnsupportedOperationException expected) {
      immutable = true;
    }
    check(immutable, "result ledger is immutable");

    System.out.println("{\"tests\":" + tests + ",\"passed\":" + passed + "}");
    if (passed != tests) System.exit(1);
  }
}
