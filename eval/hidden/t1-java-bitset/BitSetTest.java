import cf_golden.BitSet;

public final class BitSetTest {
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

  private static boolean throwsType(Runnable action, Class<? extends Throwable> type) {
    try {
      action.run();
      return false;
    } catch (Throwable error) {
      return type.isInstance(error);
    }
  }

  public static void main(String[] args) {
    BitSet bits = new BitSet(8);
    bits.set(2);
    bits.set(7);
    check(bits.get(2) && bits.get(7), "set and get");
    check(bits.count() == 2, "count");
    bits.clear(2);
    check(!bits.get(2) && bits.count() == 1, "clear");
    check(throwsType(() -> bits.get(8), IndexOutOfBoundsException.class), "range");
    check(throwsType(() -> new BitSet(0), IllegalArgumentException.class), "size");
    System.out.println("{\"tests\":" + tests + ",\"passed\":" + passed + "}");
    if (passed != tests) {
      System.exit(1);
    }
  }
}
