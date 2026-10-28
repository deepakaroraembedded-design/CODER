package main

import (
	"fmt"
	"os"

	"cf_golden/order"
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

	ledger, total, err := order.Process(nil)
	check(err == nil && ledger.ItemCount() == 0 && total.Cents == 0, "empty order")
	ledger, total, err = order.Process([]order.LineItem{
		{SKU: "A", Quantity: 2, UnitPrice: order.Money{Cents: 500}},
		{SKU: "B", Quantity: 1, UnitPrice: order.Money{Cents: 250}},
		{SKU: "A", Quantity: 1, UnitPrice: order.Money{Cents: 100}},
	})
	check(err == nil && ledger.ItemCount() == 3, "duplicate SKUs distinct")
	check(total.Cents == 1350, "total")
	_, _, err = order.Process([]order.LineItem{{SKU: "X", Quantity: 0, UnitPrice: order.Money{}}})
	check(err != nil, "rejects zero quantity")

	fmt.Printf("{\"tests\": %d, \"passed\": %d}\n", tests, passed)
	if passed != tests {
		os.Exit(1)
	}
}
