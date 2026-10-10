#include <Network/NetClient.h>
#include <core_generated.h>
#include <common/GatewayMsg.h>

#include <cstdio>

int main() {
  NetClient client;
  if (client.SendAccountAuthRequest("known_user", "correct-password", false)) {
    std::fprintf(stderr, "auth request failed without a control connection\n");
    return 1;
  }
  if (client.HasRetriableCredentials()) {
    std::fprintf(stderr, "credentials should not be retryable before successful auth\n");
    return 1;
  }
  client.SendPlayerAction(42, Protocol::PlayerActionType_ITEM_ACTION, 0, 0, 0, 2, 1);
  client.SendBlockAction(Protocol::PlayerActionType_RIGHT_MOUSE_CLICK, 0, 0, 0, 0,
                         2, 0, 42);
  client.SendInventoryAction(42, 0, 0, 0, 0, 0, 0);
  if (client.PlayerId() != 0 || client.SessionId() != 0) {
    std::fprintf(stderr, "unauthenticated NetClient acquired identity\n");
    return 1;
  }
  return 0;
}
