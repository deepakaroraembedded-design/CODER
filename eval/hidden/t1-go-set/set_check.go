package main

import (
	"fmt"
	"os"

	"cf_golden/set"
)

func main() {
	tests := 0
	passed := 0
	check := func(condition bool, name string) {
		tests++
		if condition {
			passed++
		} else {
			fmt.Fprintln(os.Stderr, "FAILED:", name)
		}
	}

	s := set.New[int]()
	s.Add(3)
	s.Add(1)
	s.Add(2)
	s.Add(2)
	check(s.Len() == 3, "deduplicates")
	check(s.Contains(2) && !s.Contains(9), "contains")
	check(fmt.Sprint(s.Sorted()) == "[1 2 3]", "sorted ascending")
	s.Remove(2)
	check(!s.Contains(2) && s.Len() == 2, "remove")

	fmt.Printf("{\"tests\": %d, \"passed\": %d}\n", tests, passed)
	if passed != tests {
		os.Exit(1)
	}
}
