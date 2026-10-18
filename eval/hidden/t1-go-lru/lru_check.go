// Hidden acceptance check for the generic LRU cache (design section 17.2).
// Run with `go run` from the integration worktree; the module path is
// cf_golden. Prints the per-case counts as a final {"tests": N, "passed": M}
// line.
package main

import (
	"fmt"
	"os"

	"cf_golden/src/lru"
)

func main() {
	tests := 0
	passed := 0
	check := func(ok bool) {
		tests++
		if ok {
			passed++
		}
	}

	// Basic put/get/len with an update.
	cache := lru.New[string, int](2)
	check(cache.Len() == 0)
	cache.Put("a", 1)
	cache.Put("b", 2)
	check(cache.Len() == 2)
	value, ok := cache.Get("a")
	check(ok && value == 1)
	_, missing := cache.Get("missing")
	check(!missing)
	cache.Put("a", 10)
	value, ok = cache.Get("a")
	check(ok && value == 10)
	check(cache.Len() == 2)

	// Get refreshes recency: after touching "a", putting "c" evicts "b".
	cache = lru.New[string, int](2)
	cache.Put("a", 1)
	cache.Put("b", 2)
	cache.Get("a")
	cache.Put("c", 3)
	_, evicted := cache.Get("b")
	check(!evicted)
	value, ok = cache.Get("a")
	check(ok && value == 1)
	value, ok = cache.Get("c")
	check(ok && value == 3)

	// Update refreshes recency too: putting "b" again makes "a" the victim.
	cache = lru.New[string, int](2)
	cache.Put("a", 1)
	cache.Put("b", 2)
	cache.Put("b", 20)
	cache.Put("c", 3)
	_, stale := cache.Get("a")
	check(!stale)
	value, ok = cache.Get("b")
	check(ok && value == 20)

	// A capacity below 1 stores nothing.
	empty := lru.New[int, string](0)
	empty.Put(1, "one")
	check(empty.Len() == 0)
	_, stored := empty.Get(1)
	check(!stored)

	// Non-string key and value types.
	byInt := lru.New[int, []string](1)
	byInt.Put(7, []string{"x", "y"})
	items, found := byInt.Get(7)
	check(found && len(items) == 2 && items[1] == "y")

	fmt.Printf("{\"tests\": %d, \"passed\": %d}\n", tests, passed)
	if passed != tests {
		os.Exit(1)
	}
}
