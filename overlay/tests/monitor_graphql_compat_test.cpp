#include "../source/monitor_graphql_compat.hpp"

#include <cassert>
#include <string>

using namespace MonitorGraphqlCompat;

int main() {
    GraphqlQueryFields fields{};
    std::string query = BuildGraphqlQuery(fields);
    assert(query.find("contentId") != std::string::npos);
    assert(query.find("advertiseData") != std::string::npos);
    assert(query.find("isConnected") != std::string::npos);
    assert(query.find(" game") == std::string::npos);
    fields.title = kGraphqlTitleLocalCommunicationId;
    assert(BuildGraphqlQuery(fields).find("localCommunicationId") != std::string::npos);
    fields.title = kGraphqlTitleCommunicationIdSnake;
    assert(BuildGraphqlQuery(fields).find("communication_id") != std::string::npos);
    fields.title = kGraphqlTitleContentIdSnake;
    fields.advertise = kGraphqlAdvertiseDataSnake;
    fields.connection = kGraphqlConnectionIsConnectedSnake;
    fields.game = kGraphqlGameScalar;
    query = BuildGraphqlQuery(fields);
    assert(query.find("content_id") != std::string::npos);
    assert(query.find("advertise_data") != std::string::npos);
    assert(query.find("is_connected") != std::string::npos);
    assert(query.find(" game ") != std::string::npos);

    fields.game = kGraphqlGameObjectName;
    assert(BuildGraphqlQuery(fields).find("game { name }") != std::string::npos);
    fields.game = kGraphqlGameObjectContentId;
    assert(BuildGraphqlQuery(fields).find("game { contentId }") != std::string::npos);
    fields.game = kGraphqlGameObjectTitleId;
    assert(BuildGraphqlQuery(fields).find("game { titleId }") != std::string::npos);
    fields.game = kGraphqlGameObjectId;
    assert(BuildGraphqlQuery(fields).find("game { id }") != std::string::npos);

    fields.game = kGraphqlGameNone;
    fields.title = kGraphqlTitleContentId;
    assert(GraphqlFailureForMessage(
        "Cannot query field \"contentId\" on type \"Room\".", fields) ==
        kGraphqlFailureTitle);
    fields.advertise = kGraphqlAdvertiseData;
    assert(GraphqlFailureForMessage(
        "Cannot query field \"advertiseData\" on type \"Room\".", fields) ==
        kGraphqlFailureAdvertise);
    fields.connection = kGraphqlConnectionIsConnected;
    assert(GraphqlFailureForMessage(
        "Cannot query field \"isConnected\" on type \"NodeInfo\".", fields) ==
        kGraphqlFailureConnection);

    fields.game = kGraphqlGameScalar;
    assert(GraphqlFailureForMessage(
        "Field \"game\" of type \"Game\" must have a selection of subfields.",
        fields) == kGraphqlFailureGameNeedsObject);
    assert(GraphqlFailureForMessage(
        "Cannot query field \"game\" on type \"Room\".", fields) ==
        kGraphqlFailureGame);
    fields.game = kGraphqlGameObjectContentId;
    assert(GraphqlFailureForMessage(
        "Cannot query field \"contentId\" on type \"Game\".", fields) ==
        kGraphqlFailureGameSubfield);

    assert(NextGraphqlTitleField(kGraphqlTitleContentId) ==
           kGraphqlTitleContentIdSnake);
    assert(NextGraphqlTitleField(kGraphqlTitleGameIdSnake) ==
           kGraphqlTitleLocalCommunicationId);
    assert(NextGraphqlTitleField(kGraphqlTitleLocalCommunicationId) ==
           kGraphqlTitleLocalCommunicationIdSnake);
    assert(NextGraphqlTitleField(kGraphqlTitleCommunicationIdSnake) ==
           kGraphqlTitleNone);
    assert(NextGraphqlAdvertiseField(kGraphqlAdvertiseData) ==
           kGraphqlAdvertiseDataSnake);
    assert(NextGraphqlConnectionField(kGraphqlConnectionIsConnected) ==
           kGraphqlConnectionIsConnectedSnake);
    assert(NextGraphqlGameObjectField(kGraphqlGameObjectName) ==
           kGraphqlGameObjectContentId);
    assert(NextGraphqlGameObjectField(kGraphqlGameObjectContentId) ==
           kGraphqlGameObjectTitleId);
    assert(NextGraphqlGameObjectField(kGraphqlGameObjectTitleId) ==
           kGraphqlGameObjectId);
    assert(NextGraphqlGameObjectField(kGraphqlGameObjectId) ==
           kGraphqlGameUnsupported);
    return 0;
}
