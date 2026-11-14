// Command connectcpp installs the SDK and creates independent C++ RPC projects.
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"

	"github.com/happenings-dk/connect-cc/internal/tool"
)

func main() {
	code := 0
	func() {
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
		defer stop()
		if err := tool.Run(ctx, os.Args[1:], os.Stdout, os.Stderr); err != nil {
			fmt.Fprintln(os.Stderr, "connectcpp:", err)
			code = 1
		}
	}()
	os.Exit(code)
}
