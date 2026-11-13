// Command protoc-gen-connect-cpp generates C++ ConnectRPC service bindings.
package main

import (
	"fmt"
	"io"
	"os"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/pluginpb"

	"connectcpp.local/connect-cc/internal/codegen"
)

func main() {
	if err := run(os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(input io.Reader, output io.Writer) error {
	const maxRequest = 64 << 20
	data, err := io.ReadAll(io.LimitReader(input, maxRequest+1))
	if err != nil {
		return fmt.Errorf("read request: %w", err)
	}
	if len(data) > maxRequest {
		return fmt.Errorf("request exceeds %d bytes", maxRequest)
	}
	var request pluginpb.CodeGeneratorRequest
	if err := proto.Unmarshal(data, &request); err != nil {
		return fmt.Errorf("decode request: %w", err)
	}
	response, err := codegen.Generate(&request)
	if err != nil {
		response = &pluginpb.CodeGeneratorResponse{Error: proto.String(err.Error())}
	}
	data, err = proto.Marshal(response)
	if err != nil {
		return fmt.Errorf("encode response: %w", err)
	}
	if n, err := output.Write(data); err != nil {
		return fmt.Errorf("write response: %w", err)
	} else if n != len(data) {
		return fmt.Errorf("write response: %w", io.ErrShortWrite)
	}
	return nil
}
