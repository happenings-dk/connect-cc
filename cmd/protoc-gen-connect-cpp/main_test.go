package main

import (
	"bytes"
	"io"
	"testing"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/pluginpb"
)

func TestRunReportsPluginErrorInResponse(t *testing.T) {
	data, err := proto.Marshal(&pluginpb.CodeGeneratorRequest{Parameter: proto.String("unknown=true")})
	if err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := run(bytes.NewReader(data), &output); err != nil {
		t.Fatal(err)
	}
	var response pluginpb.CodeGeneratorResponse
	if err := proto.Unmarshal(output.Bytes(), &response); err != nil {
		t.Fatal(err)
	}
	if response.GetError() == "" {
		t.Fatal("missing protocol error response")
	}
}
func TestRunRejectsMalformedInput(t *testing.T) {
	if err := run(bytes.NewReader([]byte{255}), io.Discard); err == nil {
		t.Fatal("expected malformed request failure")
	}
}
