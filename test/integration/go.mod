module github.com/gtnh-platform/integration-tests

go 1.22

require (
	github.com/google/flatbuffers v24.3.25+incompatible
	github.com/gtnh-platform/protocol/generated/go v0.0.0
)

replace github.com/gtnh-platform/protocol/generated/go => ../../src/protocol/generated/go
