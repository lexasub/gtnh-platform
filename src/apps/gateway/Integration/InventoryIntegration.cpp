#include "InventoryIntegration.h"
#include <stdexcept>
#include <flatbuffers/flatbuffers.h>
#include "protocol/core_generated.h"

InventoryIntegration::InventoryIntegration(std::shared_ptr<IPlayerInventoryStorage> playerInventoryStorage,
                                           std::shared_ptr<IEntityStateStorage> entityStateStorage)
    : playerInventoryStorage_(std::move(playerInventoryStorage))
    , entityStateStorage_(std::move(entityStateStorage))
{
    if (!playerInventoryStorage_) {
        throw std::invalid_argument("playerInventoryStorage cannot be null");
    }
    if (!entityStateStorage_) {
        throw std::invalid_argument("entityStateStorage cannot be null");
    }
}

bool InventoryIntegration::LoadPlayerInventory(uint64_t playerId, std::vector<uint8_t>& inventoryData) {
    return playerInventoryStorage_->LoadPlayerInventory(playerId, inventoryData);
}

bool InventoryIntegration::SavePlayerInventory(uint64_t playerId, const std::vector<uint8_t>& inventoryData) {
    return playerInventoryStorage_->SavePlayerInventory(playerId, inventoryData);
}

bool InventoryIntegration::GetPlayerPosition(uint64_t playerId, int32_t& x, int32_t& y, int32_t& z) {
    return playerInventoryStorage_->GetPlayerPosition(playerId, x, y, z);
}

bool InventoryIntegration::SavePlayerPosition(uint64_t playerId, int32_t x, int32_t y, int32_t z) {
    return playerInventoryStorage_->SavePlayerPosition(playerId, x, y, z);
}

bool InventoryIntegration::LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                          uint16_t entityType, std::vector<uint8_t>& stateData) {
    return entityStateStorage_->LoadEntityState(dimension, x, y, z, entityType, stateData);
}

bool InventoryIntegration::SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                          uint16_t entityType, const std::vector<uint8_t>& stateData) {
    return entityStateStorage_->SaveEntityState(dimension, x, y, z, entityType, stateData);
}

bool InventoryIntegration::DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                            uint16_t entityType) {
    return entityStateStorage_->DeleteEntityState(dimension, x, y, z, entityType);
}

bool InventoryIntegration::ProcessInventoryUpdateRequest(const uint8_t* data, size_t len, 
                                                        std::vector<uint8_t>& responseData) {
    // Verify the FlatBuffers data
    auto verifier = flatbuffers::Verifier(data, len);
    if (!verifier.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) {
        return false;
    }
    
    auto update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data);
    
    // Serialize slots to GetInventoryResp format for persistent storage
    flatbuffers::FlatBufferBuilder serializer;
    std::vector<flatbuffers::Offset<Protocol::InventorySlot>> slotOffsets;
    if (auto* slots = update->slots()) {
        for (size_t i = 0; i < slots->size(); ++i) {
            auto* slot = slots->Get(i);
            slotOffsets.push_back(Protocol::CreateInventorySlot(
                serializer, slot->item_id(), slot->count(), slot->meta()));
        }
    }
    auto storedResp = Protocol::CreateGetInventoryResp(
        serializer, serializer.CreateVector(slotOffsets));
    serializer.Finish(storedResp);
    
    std::vector<uint8_t> serializedData(
        serializer.GetBufferPointer(),
        serializer.GetBufferPointer() + serializer.GetSize());
    
    if (!playerInventoryStorage_->SavePlayerInventory(update->player_id(), serializedData)) {
        return false;
    }
    
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<Protocol::InventorySlot>> respSlots;
    if (auto* slots = update->slots()) {
        for (size_t i = 0; i < slots->size(); ++i) {
            auto* slot = slots->Get(i);
            respSlots.push_back(Protocol::CreateInventorySlot(
                fbb, slot->item_id(), slot->count(), slot->meta()));
        }
    }
    auto resp = Protocol::CreateGetInventoryResp(fbb, fbb.CreateVector(respSlots));
    fbb.Finish(resp);
    
    responseData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    return true;
}

bool InventoryIntegration::ProcessInventorySnapshotRequest(const uint8_t* data, size_t len,
                                                          std::vector<uint8_t>& responseData) {
    // Verify the FlatBuffers data
    auto verifier = flatbuffers::Verifier(data, len);
    if (!verifier.VerifyBuffer<Protocol::GetInventorySnapshotReq>(nullptr)) {
        return false;
    }
    
    auto req = flatbuffers::GetRoot<Protocol::GetInventorySnapshotReq>(data);
    
    std::vector<uint8_t> inventoryData;
    if (!playerInventoryStorage_->LoadPlayerInventory(req->player_id(), inventoryData)) {
        return false;
    }
    
    // Verify stored data is a valid GetInventoryResp
    auto storedVerifier = flatbuffers::Verifier(inventoryData.data(), inventoryData.size());
    if (!storedVerifier.VerifyBuffer<Protocol::GetInventoryResp>(nullptr)) {
        return false;
    }
    
    auto stored = flatbuffers::GetRoot<Protocol::GetInventoryResp>(inventoryData.data());
    
    flatbuffers::FlatBufferBuilder fbb;
    std::vector<flatbuffers::Offset<Protocol::InventorySlot>> mainSlots;
    if (auto* inv = stored->inventory()) {
        for (size_t i = 0; i < inv->size(); ++i) {
            auto* slot = inv->Get(i);
            mainSlots.push_back(Protocol::CreateInventorySlot(
                fbb, slot->item_id(), slot->count(), slot->meta()));
        }
    }
    auto resp = Protocol::CreateGetInventorySnapshotResp(
        fbb, fbb.CreateVector(mainSlots));
    fbb.Finish(resp);
    
    responseData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
    return true;
}

bool InventoryIntegration::ProcessEntityStateRequest(const uint8_t* data, size_t len,
                                                    std::vector<uint8_t>& responseData) {
    // Verify the FlatBuffers data
    auto verifier = flatbuffers::Verifier(data, len);
    bool isGetReq = verifier.VerifyBuffer<Protocol::GetEntityStateReq>(nullptr);
    bool isSetReq = verifier.VerifyBuffer<Protocol::SetEntityStateReq>(nullptr);
    
    if (!isGetReq && !isSetReq) {
        return false;
    }
    
    if (isGetReq) {
        auto req = flatbuffers::GetRoot<Protocol::GetEntityStateReq>(data);
        std::vector<uint8_t> stateData;
        if (!entityStateStorage_->LoadEntityState(
                req->dimension(), req->x(), req->y(), req->z(), 
                req->entity_type(), stateData)) {
            return false;
        }
        
        flatbuffers::FlatBufferBuilder fbb;
        auto resp = Protocol::CreateGetEntityStateResp(
            fbb, 
            fbb.CreateVector(stateData)
        );
        fbb.Finish(resp);
        
        responseData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        return true;
    } else { // isSetReq
        auto req = flatbuffers::GetRoot<Protocol::SetEntityStateReq>(data);
        if (!entityStateStorage_->SaveEntityState(
                req->dimension(), req->x(), req->y(), req->z(), 
                req->entity_type(), req->state())) {
            return false;
        }
        
        flatbuffers::FlatBufferBuilder fbb;
        auto ack = Protocol::CreateEntityStateAck(fbb, true);
        fbb.Finish(ack);
        
        responseData.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        return true;
    }
}

bool InventoryIntegration::IsPlayerBoundRequest(const uint8_t* data, size_t len) {
    // Check if this is a player-bound request (MetaDB)
    auto verifier = flatbuffers::Verifier(data, len);
    return verifier.VerifyBuffer<Protocol::GetInventoryReq>(nullptr) ||
           verifier.VerifyBuffer<Protocol::SetInventorySlotReq>(nullptr) ||
           verifier.VerifyBuffer<Protocol::GetInventorySnapshotReq>(nullptr) ||
           verifier.VerifyBuffer<Protocol::InventoryUpdate>(nullptr);
}

bool InventoryIntegration::IsWorldBoundRequest(const uint8_t* data, size_t len) {
    // Check if this is a world-bound request (EntityStateStore)
    auto verifier = flatbuffers::Verifier(data, len);
    return verifier.VerifyBuffer<Protocol::GetEntityStateReq>(nullptr) ||
           verifier.VerifyBuffer<Protocol::SetEntityStateReq>(nullptr);
}