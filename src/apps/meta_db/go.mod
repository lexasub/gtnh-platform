module github.com/gtnh-platform/metadb

go 1.22

require (
	github.com/google/flatbuffers v24.3.25+incompatible
	github.com/gtnh-platform/protocol/generated/go v0.0.0
	github.com/gtnh/platform/gtnh-common v0.0.0
	github.com/mattn/go-sqlite3 v1.14.18
)

require (
	golang.org/x/crypto v0.29.0 // indirect
	golang.org/x/sys v0.27.0 // indirect
)

replace github.com/gtnh-platform/protocol/generated/go => ../../protocol/generated/go

replace github.com/gtnh/platform/gtnh-common => ../../libs/gtnh-common-go
