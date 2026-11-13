// Package interop tests generated C++ bindings against connect-go in both directions.
package interop

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"connectrpc.com/connect"
	"google.golang.org/protobuf/types/known/wrapperspb"
)

const procedure = "/interop.v1.Echo/Unary"

func binary(t *testing.T) string {
	t.Helper()
	name := os.Getenv("CONNECTCPP_INTEROP_BINARY")
	if name == "" {
		t.Skip("run through CTest Interop.Go or set CONNECTCPP_INTEROP_BINARY")
	}
	return name
}

func TestCppClientAgainstGoServer(t *testing.T) {
	program := binary(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	protocols := new(http.Protocols)
	protocols.SetUnencryptedHTTP2(true)
	server := &http.Server{
		Protocols: protocols, ReadHeaderTimeout: 5 * time.Second,
		Handler: connect.NewUnaryHandler(procedure, func(_ context.Context, request *connect.Request[wrapperspb.StringValue]) (*connect.Response[wrapperspb.StringValue], error) {
			if request.Header().Get("request-id") != "cpp" {
				return nil, connect.NewError(connect.CodeInvalidArgument, errors.New("missing request metadata"))
			}
			if request.Msg.Value == "error" {
				result := connect.NewError(connect.CodePermissionDenied, errors.New("denied"))
				detail, err := connect.NewErrorDetail(wrapperspb.String("reason"))
				if err != nil {
					return nil, err
				}
				result.AddDetail(detail)
				// Connect unary errors have one metadata map. This prefix requests
				// trailing wire metadata for the cross-language assertion.
				result.Meta().Set("Trailer-Cost", "1")
				return nil, result
			}
			response := connect.NewResponse(wrapperspb.String(request.Msg.Value))
			response.Header().Set("request-id", request.Header().Get("request-id"))
			response.Trailer().Set("cost", "1")
			return response, nil
		}, connect.WithCompressMinBytes(0)),
	}
	done := make(chan error, 1)
	go func() { done <- server.Serve(listener) }()
	t.Cleanup(func() {
		if err := server.Close(); err != nil {
			t.Error(err)
		}
		if err := <-done; !errors.Is(err, http.ErrServerClosed) {
			t.Errorf("server: %v", err)
		}
	})
	_, port, err := net.SplitHostPort(listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, program, "client", port).CombinedOutput()
	if err != nil {
		t.Fatalf("C++ client: %v\n%s", err, output)
	}
}

func TestGoClientAgainstCppServer(t *testing.T) {
	program := binary(t)
	ctx, cancel := context.WithTimeout(t.Context(), 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, program, "server")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	// Always reap the helper, including assertion failures and startup timeouts.
	defer func() {
		cancel()
		_ = cmd.Wait()
	}()
	scanner := bufio.NewScanner(stdout)
	if !scanner.Scan() {
		t.Fatalf("C++ server did not announce a port: %v", scanner.Err())
	}
	port, err := strconv.ParseUint(scanner.Text(), 10, 16)
	if err != nil || port == 0 {
		t.Fatalf("invalid server port %q", scanner.Text())
	}
	protocols := new(http.Protocols)
	protocols.SetUnencryptedHTTP2(true)
	transport := &http.Transport{Protocols: protocols}
	defer transport.CloseIdleConnections()
	httpClient := &http.Client{Transport: transport, Timeout: 5 * time.Second}
	for _, json := range []bool{false, true} {
		for _, compressed := range []bool{false, true} {
			t.Run(fmt.Sprintf("json=%v/gzip=%v", json, compressed), func(t *testing.T) {
				options := []connect.ClientOption{connect.WithCompressMinBytes(0)}
				if json {
					options = append(options, connect.WithProtoJSON())
				}
				if compressed {
					options = append(options, connect.WithSendCompression("gzip"))
				}
				client := connect.NewClient[wrapperspb.StringValue, wrapperspb.StringValue](httpClient, fmt.Sprintf("http://127.0.0.1:%d%s", port, procedure), options...)
				request := connect.NewRequest(wrapperspb.String(strings.Repeat("x", 70000)))
				request.Header().Set("request-id", "go")
				response, err := client.CallUnary(ctx, request)
				if err != nil {
					t.Fatalf("unary: %v", err)
				}
				if response.Msg.Value != request.Msg.Value {
					t.Fatal("response payload differs")
				}
				if response.Header().Get("request-id") != "go" || response.Trailer().Get("cost") != "1" {
					t.Fatalf("metadata: %v, %v", response.Header(), response.Trailer())
				}
				_, err = client.CallUnary(ctx, connect.NewRequest(wrapperspb.String("error")))
				var rpcError *connect.Error
				if !errors.As(err, &rpcError) || rpcError.Code() != connect.CodePermissionDenied || rpcError.Message() != "denied" {
					t.Fatalf("error response: %v", err)
				}
				if len(rpcError.Details()) != 1 {
					t.Fatalf("details: %v", rpcError.Details())
				}
				detail, err := rpcError.Details()[0].Value()
				if err != nil {
					t.Fatal(err)
				}
				text, ok := detail.(*wrapperspb.StringValue)
				if !ok || text.Value != "reason" {
					t.Fatalf("detail: %v", detail)
				}
			})
		}
	}
	transport.CloseIdleConnections()
	if err := cmd.Wait(); err != nil {
		t.Fatalf("C++ server: %v\n%s", err, stderr.String())
	}
}
