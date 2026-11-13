package codegen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"google.golang.org/protobuf/encoding/prototext"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/pluginpb"
)

func fixture(t *testing.T) *pluginpb.CodeGeneratorRequest {
	t.Helper()
	data, err := os.ReadFile("testdata/descriptors.textproto")
	if err != nil {
		t.Fatal(err)
	}
	var files descriptorpb.FileDescriptorSet
	if err := prototext.Unmarshal(data, &files); err != nil {
		t.Fatal(err)
	}
	return &pluginpb.CodeGeneratorRequest{
		FileToGenerate: []string{"test/v1/echo.proto", "shared/v1/types.proto"},
		ProtoFile:      files.File,
	}
}

func TestGenerateGolden(t *testing.T) {
	t.Parallel()
	request := fixture(t)
	response, err := Generate(request)
	if err != nil {
		t.Fatal(err)
	}
	if len(response.File) != 2 {
		t.Fatalf("generated %d files, want 2", len(response.File))
	}
	for _, file := range response.File {
		want, err := os.ReadFile(filepath.Join("testdata", filepath.Base(file.GetName())+".golden"))
		if err != nil {
			t.Fatal(err)
		}
		if file.GetContent() != string(want) {
			t.Errorf("%s differs from golden:\n%s", file.GetName(), file.GetContent())
		}
	}
	if response.GetSupportedFeatures()&uint64(pluginpb.CodeGeneratorResponse_FEATURE_PROTO3_OPTIONAL) == 0 {
		t.Error("proto3 optional support not advertised")
	}
	again, err := Generate(request)
	if err != nil || !proto.Equal(response, again) {
		t.Fatalf("generation is nondeterministic: %v", err)
	}
}

func TestGenerateRejectsUnsupportedInputs(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*pluginpb.CodeGeneratorRequest)
		want   string
	}{
		{name: "parameter", mutate: func(r *pluginpb.CodeGeneratorRequest) { r.Parameter = proto.String("typo=true") }, want: "unsupported parameter"},
		{name: "missing file", mutate: func(r *pluginpb.CodeGeneratorRequest) { r.FileToGenerate = []string{"missing.proto"} }, want: "find"},
		{name: "path traversal", mutate: func(r *pluginpb.CodeGeneratorRequest) { r.FileToGenerate = []string{"../escape.proto"} }, want: "invalid proto path"},
		{name: "duplicate", mutate: func(r *pluginpb.CodeGeneratorRequest) {
			r.FileToGenerate = append(r.FileToGenerate, r.FileToGenerate[0])
		}, want: "duplicate input"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			request := fixture(t)
			tt.mutate(request)
			_, err := Generate(request)
			if err == nil || !strings.Contains(err.Error(), tt.want) {
				t.Fatalf("Generate() error = %v, want containing %q", err, tt.want)
			}
		})
	}
}

func TestStreamingShapesGenerate(t *testing.T) {
	for _, tt := range []struct {
		name           string
		client, server bool
		wantCall       string
	}{
		{name: "client", client: true, wantCall: "ClientStream"},
		{name: "server", server: true, wantCall: "ServerStream"},
		{name: "bidi", client: true, server: true, wantCall: "BidiStream"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			request := fixture(t)
			for _, file := range request.ProtoFile {
				if len(file.Service) == 0 {
					continue
				}
				method := file.Service[0].Method[0]
				method.ClientStreaming = proto.Bool(tt.client)
				method.ServerStreaming = proto.Bool(tt.server)
			}
			response, err := Generate(request)
			if err != nil {
				t.Fatal(err)
			}
			const typeArgs = `<::shared::v1::Outer_Nested, ::shared::v1::Outer_Nested>("/test.v1.Echo/Say"`
			for _, want := range []string{"client_." + tt.wantCall + typeArgs, "server.Register" + tt.wantCall + typeArgs} {
				if !strings.Contains(response.File[1].GetContent(), want) {
					t.Errorf("generated source missing %q", want)
				}
			}
		})
	}
}

func TestHeaderGuardsDistinguishNormalizedPaths(t *testing.T) {
	guards := make(map[string]bool)
	for _, filename := range []string{"foo-bar.proto", "foo_bar.proto"} {
		request := fixture(t)
		for _, file := range request.ProtoFile {
			if file.GetName() == "test/v1/echo.proto" {
				file.Name = proto.String(filename)
			}
		}
		request.FileToGenerate = []string{filename}
		response, err := Generate(request)
		if err != nil {
			t.Fatal(err)
		}
		guard := strings.Split(response.File[0].GetContent(), "\n")[1]
		if guards[guard] {
			t.Fatalf("duplicate include guard: %s", guard)
		}
		guards[guard] = true
	}
}

func TestGenerateIdempotency(t *testing.T) {
	t.Parallel()
	for _, tt := range []struct {
		name string
		give descriptorpb.MethodOptions_IdempotencyLevel
		want string
	}{
		{name: "side effect free", give: descriptorpb.MethodOptions_NO_SIDE_EFFECTS, want: "kNoSideEffects"},
		{name: "idempotent only", give: descriptorpb.MethodOptions_IDEMPOTENT, want: "kIdempotent"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			request := fixture(t)
			for _, file := range request.ProtoFile {
				if file.GetName() == "test/v1/echo.proto" {
					file.Service[0].Method[0].Options = &descriptorpb.MethodOptions{IdempotencyLevel: tt.give.Enum()}
				}
			}
			response, err := Generate(request)
			if err != nil {
				t.Fatal(err)
			}
			var source string
			for _, file := range response.File {
				if strings.HasSuffix(file.GetName(), ".connect.cc") {
					source = file.GetContent()
				}
			}
			if count := strings.Count(source, "::connectrpc::Idempotency::"+tt.want); count != 2 {
				t.Errorf("idempotency in client and server: got %d references, want 2:\n%s", count, source)
			}
		})
	}
}
