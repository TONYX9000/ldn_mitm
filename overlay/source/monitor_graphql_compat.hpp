#pragma once

#include <string>
#include <string.h>

/* Pure GraphQL schema negotiation helpers. Kept independent of Switch APIs so
 * host tests can exercise query construction and field-error fallbacks. */
namespace MonitorGraphqlCompat {

enum GraphqlTitleField : unsigned char {
    kGraphqlTitleNone = 0,
    kGraphqlTitleContentId,
    kGraphqlTitleContentIdSnake,
    kGraphqlTitleId,
    kGraphqlTitleIdSnake,
    kGraphqlTitleProgramId,
    kGraphqlTitleProgramIdSnake,
    kGraphqlTitleApplicationId,
    kGraphqlTitleApplicationIdSnake,
    kGraphqlTitleGameId,
    kGraphqlTitleGameIdSnake,
    kGraphqlTitleLocalCommunicationId,
    kGraphqlTitleLocalCommunicationIdSnake,
    kGraphqlTitleCommunicationId,
    kGraphqlTitleCommunicationIdSnake,
};
enum GraphqlAdvertiseField : unsigned char {
    kGraphqlAdvertiseNone = 0,
    kGraphqlAdvertiseData,
    kGraphqlAdvertiseDataSnake,
};
enum GraphqlConnectionField : unsigned char {
    kGraphqlConnectionNone = 0,
    kGraphqlConnectionIsConnected,
    kGraphqlConnectionIsConnectedSnake,
};
enum GraphqlGameField : unsigned char {
    kGraphqlGameNone = 0,
    kGraphqlGameScalar,
    kGraphqlGameObjectName,
    kGraphqlGameObjectContentId,
    kGraphqlGameObjectTitleId,
    kGraphqlGameObjectId,
    kGraphqlGameUnsupported,
};
enum GraphqlFieldFailure : unsigned char {
    kGraphqlFailureNone = 0,
    kGraphqlFailureTitle,
    kGraphqlFailureAdvertise,
    kGraphqlFailureConnection,
    kGraphqlFailureGameNeedsObject,
    kGraphqlFailureGame,
    kGraphqlFailureGameSubfield,
    kGraphqlFailureOther,
};
struct GraphqlQueryFields {
    unsigned char title = kGraphqlTitleContentId;
    unsigned char advertise = kGraphqlAdvertiseData;
    unsigned char connection = kGraphqlConnectionIsConnected;
    unsigned char game = kGraphqlGameNone;
    bool gameSubfieldsDone = false;
};

inline const char *GraphqlTitleFieldName(unsigned char field) {
    switch (field) {
        case kGraphqlTitleContentId: return "contentId";
        case kGraphqlTitleContentIdSnake: return "content_id";
        case kGraphqlTitleId: return "titleId";
        case kGraphqlTitleIdSnake: return "title_id";
        case kGraphqlTitleProgramId: return "programId";
        case kGraphqlTitleProgramIdSnake: return "program_id";
        case kGraphqlTitleApplicationId: return "applicationId";
        case kGraphqlTitleApplicationIdSnake: return "application_id";
        case kGraphqlTitleGameId: return "gameId";
        case kGraphqlTitleGameIdSnake: return "game_id";
        case kGraphqlTitleLocalCommunicationId: return "localCommunicationId";
        case kGraphqlTitleLocalCommunicationIdSnake: return "local_communication_id";
        case kGraphqlTitleCommunicationId: return "communicationId";
        case kGraphqlTitleCommunicationIdSnake: return "communication_id";
        default: return nullptr;
    }
}

inline const char *GraphqlAdvertiseFieldName(unsigned char field) {
    switch (field) {
        case kGraphqlAdvertiseData: return "advertiseData";
        case kGraphqlAdvertiseDataSnake: return "advertise_data";
        default: return nullptr;
    }
}

inline const char *GraphqlConnectionFieldName(unsigned char field) {
    switch (field) {
        case kGraphqlConnectionIsConnected: return "isConnected";
        case kGraphqlConnectionIsConnectedSnake: return "is_connected";
        default: return nullptr;
    }
}

inline const char *GraphqlGameSubfieldName(unsigned char gameField) {
    switch (gameField) {
        case kGraphqlGameObjectName: return "name";
        case kGraphqlGameObjectContentId: return "contentId";
        case kGraphqlGameObjectTitleId: return "titleId";
        case kGraphqlGameObjectId: return "id";
        default: return nullptr;
    }
}

inline std::string BuildGraphqlQuery(const GraphqlQueryFields &fields) {
    std::string query = "query PublicRoomSnapshot { serverInfo { online idle } room { sessionId";
    if (const char *field = GraphqlTitleFieldName(fields.title)) {
        query += ' ';
        query += field;
    }
    if (const char *field = GraphqlAdvertiseFieldName(fields.advertise)) {
        query += ' ';
        query += field;
    }
    query += " hostPlayerName nodeCount nodeCountMax";
    if (fields.game == kGraphqlGameScalar)
        query += " game";
    else if (const char *subfield = GraphqlGameSubfieldName(fields.game)) {
        query += " game { ";
        query += subfield;
        query += " }";
    }
    query += " nodes { playerName";
    if (const char *field = GraphqlConnectionFieldName(fields.connection)) {
        query += ' ';
        query += field;
    }
    query += " } } }";
    return query;
}

inline bool GraphqlUnknownFieldMessage(const char *message, const char *field) {
    if (!message || !field)
        return false;
    const char *p = strstr(message, "Cannot query field");
    if (!p)
        p = strstr(message, "cannot query field");
    if (!p)
        return false;
    p += strlen("Cannot query field");
    while (*p == ' ')
        ++p;
    const char quote = *p;
    if (quote != '"' && quote != '\'' && quote != '`')
        return false;
    ++p;
    const size_t fieldLen = strlen(field);
    return strncmp(p, field, fieldLen) == 0 && p[fieldLen] == quote;
}

inline GraphqlFieldFailure GraphqlFailureForMessage(
        const char *message, const GraphqlQueryFields &fields) {
    if (!message)
        return kGraphqlFailureOther;
    if (fields.game == kGraphqlGameScalar &&
        strstr(message, "must have a selection of subfields") &&
        strstr(message, "game"))
        return kGraphqlFailureGameNeedsObject;
    const char *gameSubfield = GraphqlGameSubfieldName(fields.game);
    if (gameSubfield && GraphqlUnknownFieldMessage(message, gameSubfield))
        return kGraphqlFailureGameSubfield;
    if (fields.game != kGraphqlGameNone && fields.game != kGraphqlGameUnsupported &&
        GraphqlUnknownFieldMessage(message, "game"))
        return kGraphqlFailureGame;
    const char *titleField = GraphqlTitleFieldName(fields.title);
    if (titleField && GraphqlUnknownFieldMessage(message, titleField))
        return kGraphqlFailureTitle;
    const char *advertiseField = GraphqlAdvertiseFieldName(fields.advertise);
    if (advertiseField && GraphqlUnknownFieldMessage(message, advertiseField))
        return kGraphqlFailureAdvertise;
    const char *connectionField = GraphqlConnectionFieldName(fields.connection);
    if (connectionField && GraphqlUnknownFieldMessage(message, connectionField))
        return kGraphqlFailureConnection;
    return kGraphqlFailureOther;
}

inline unsigned char NextGraphqlTitleField(unsigned char field) {
    return field < kGraphqlTitleCommunicationIdSnake
        ? static_cast<unsigned char>(field + 1)
        : static_cast<unsigned char>(kGraphqlTitleNone);
}
inline unsigned char NextGraphqlAdvertiseField(unsigned char field) {
    return field < kGraphqlAdvertiseDataSnake
        ? static_cast<unsigned char>(field + 1)
        : static_cast<unsigned char>(kGraphqlAdvertiseNone);
}
inline unsigned char NextGraphqlConnectionField(unsigned char field) {
    return field < kGraphqlConnectionIsConnectedSnake
        ? static_cast<unsigned char>(field + 1)
        : static_cast<unsigned char>(kGraphqlConnectionNone);
}
inline unsigned char NextGraphqlGameObjectField(unsigned char field) {
    return field < kGraphqlGameObjectId
        ? static_cast<unsigned char>(field + 1)
        : static_cast<unsigned char>(kGraphqlGameUnsupported);
}

} // namespace MonitorGraphqlCompat
