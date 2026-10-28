package main

import (
	"fmt"
	"os"
	"strings"

	"cf_golden/pathguard"
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

	root := "/srv/data"
	joined, err := pathguard.Join(root, "docs/readme.md")
	check(err == nil && strings.HasPrefix(joined, root+"/"), "accepts relative path")
	_, err = pathguard.Join(root, "../etc/passwd")
	check(err != nil, "rejects parent traversal")
	_, err = pathguard.Join(root, "/etc/passwd")
	check(err != nil, "rejects absolute path")
	_, err = pathguard.Join(root, "docs/../../secret")
	check(err != nil, "rejects nested traversal")
	joined, err = pathguard.Join(root, "docs/./readme.md")
	check(err == nil && joined == "/srv/data/docs/readme.md", "cleans dot segments")
	_, err = pathguard.Join(root, "")
	check(err != nil, "rejects empty path")

	fmt.Printf("{\"tests\": %d, \"passed\": %d}\n", tests, passed)
	if passed != tests {
		os.Exit(1)
	}
}
