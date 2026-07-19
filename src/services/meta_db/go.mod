module github.com/gtnh-platform/metadb

go 1.22

require (
	github.com/google/flatbuffers v24.3.25+incompatible
	github.com/gtnh-platform/protocol/generated/go v0.0.0
	github.com/gtnh/platform/gtnh-common v0.0.0
	github.com/mattn/go-sqlite3 v1.14.18
)

replace github.com/gtnh-platform/protocol/generated/go => ../../protocol/generated/go

replace github.com/gtnh/platform/gtnh-common => ../../libs/gtnh-common-go
