package main

import (
	"fmt"
	"os"
	"sync"
	"sync/atomic"

	"cf_golden/pool"
	"cf_golden/queue"
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

	q := queue.New[int](2)
	check(q.Push(1) == nil, "push one")
	check(q.Push(2) == nil, "push two")
	check(q.Push(3) != nil, "push into full queue")
	first, ok := q.Pop()
	check(ok && first == 1, "FIFO pop")
	q.Close()
	check(q.Push(4) != nil, "closed queue rejects push")

	q2 := queue.New[int](8)
	for i := 0; i < 6; i++ {
		check(q2.Push(i) == nil, "fill queue")
	}
	var seen int64
	var wg sync.WaitGroup
	wg.Add(6)
	p := pool.New[int](3, q2)
	p.Start(func(value int) {
		atomic.AddInt64(&seen, 1)
		wg.Done()
	})
	q2.Close()
	p.Close()
	wg.Wait()
	check(atomic.LoadInt64(&seen) == 6, "processes every item exactly once")
	check(q2.Len() == 0, "queue drains")

	fmt.Printf("{\"tests\": %d, \"passed\": %d}\n", tests, passed)
	if passed != tests {
		os.Exit(1)
	}
}
