# FlatBuffers schema → Go code — shared protocol from src/protocol/
set(FBS_DIR ${CMAKE_SOURCE_DIR}/src/protocol)
set(FBS_GO_OUT_DIR ${CMAKE_SOURCE_DIR}/src/protocol/generated/go)
file(MAKE_DIRECTORY ${FBS_GO_OUT_DIR})

add_custom_command(
    OUTPUT
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBFrame.go
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBMessage.go
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBReply.go
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBRequest.go
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBResponse.go
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBPayload.go
        ${FBS_GO_OUT_DIR}/Protocol/InventoryUpdate.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestStatus.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestProgressUpdate.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestEntry.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestCompleted.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestCompletedNotification.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestUnlockNotification.go
        ${FBS_GO_OUT_DIR}/Protocol/QuestUnlocked.go
    COMMAND flatbuffers::flatc
        --go
        -I ${FBS_DIR}
        -o ${FBS_GO_OUT_DIR}
        ${FBS_DIR}/meta_db.fbs
        ${FBS_DIR}/core.fbs
        ${FBS_DIR}/quest.fbs
    DEPENDS flatbuffers::flatc
            ${FBS_DIR}/core.fbs
            ${FBS_DIR}/meta_db.fbs
            ${FBS_DIR}/quest.fbs
    COMMENT "Generating FlatBuffers Go code for meta_db"
)

# MetaDB is a Go service - build with go build
add_custom_command(
    OUTPUT metadbd
    COMMAND go build -o metadbd .
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/main.go
        ${CMAKE_CURRENT_SOURCE_DIR}/go.mod
        ${FBS_GO_OUT_DIR}/Protocol/MetaDBFrame.go
    COMMENT "Building MetaDB Go service"
)

add_custom_target(metadbd_target ALL
    DEPENDS metadbd
)
