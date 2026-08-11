module github.com/gtnh-platform/loadtest

go 1.22

require (
	github.com/google/flatbuffers v24.3.25+incompatible
	github.com/gtnh-platform/protocol/generated/go v0.0.0-00010101000000-000000000000
)

replace github.com/gtnh-platform/protocol/generated/go => ../../src/protocol/generated/go
