import cf_golden.BoundedStack;
import java.util.NoSuchElementException;

public final class BoundedStackTest {
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
    BoundedStack<Integer> stack = new BoundedStack<>(2);
    check(stack.isEmpty(), "new stack is empty");
    stack.push(1);
    stack.push(2);
    check(stack.size() == 2, "size after two pushes");
    check(stack.peek() == 2, "peek is LIFO");
    check(throwsType(() -> stack.push(3), IllegalStateException.class),
          "push into full stack throws");
    check(stack.pop() == 2 && stack.pop() == 1, "pop is LIFO");
    check(stack.isEmpty(), "empty after pops");
    check(throwsType(stack::pop, NoSuchElementException.class),
          "pop on empty throws");
    check(throwsType(() -> new BoundedStack<Integer>(0),
                     IllegalArgumentException.class),
          "non-positive capacity throws");
    System.out.println("{\"tests\":" + tests + ",\"passed\":" + passed + "}");
    if (passed != tests) {
      System.exit(1);
    }
  }
}
