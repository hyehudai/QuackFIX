#pragma once

#include <cstddef> // for size_t

namespace duckdb {

// Centralized hot tag definitions for FIX protocol
// These 19 tags are hardcoded for performance-critical parsing
// All other tags are routed through the dictionary-driven other_tags map
namespace FixHotTags {

// Hot tag constants - the 19 most commonly used FIX tags
constexpr int MSG_TYPE = 35;       // MsgType
constexpr int SENDER_COMP_ID = 49; // SenderCompID
constexpr int TARGET_COMP_ID = 56; // TargetCompID
constexpr int MSG_SEQ_NUM = 34;    // MsgSeqNum
constexpr int SENDING_TIME = 52;   // SendingTime
constexpr int CL_ORD_ID = 11;      // ClOrdID
constexpr int ORDER_ID = 37;       // OrderID
constexpr int EXEC_ID = 17;        // ExecID
constexpr int SYMBOL = 55;         // Symbol
constexpr int SIDE = 54;           // Side
constexpr int EXEC_TYPE = 150;     // ExecType
constexpr int ORD_STATUS = 39;     // OrdStatus
constexpr int PRICE = 44;          // Price
constexpr int ORDER_QTY = 38;      // OrderQty
constexpr int CUM_QTY = 14;        // CumQty
constexpr int LEAVES_QTY = 151;    // LeavesQty
constexpr int LAST_PX = 31;        // LastPx
constexpr int LAST_QTY = 32;       // LastQty
constexpr int TEXT = 58;           // Text

// Array of all hot tags for iteration/validation
constexpr int ALL_TAGS[] = {MSG_TYPE, SENDER_COMP_ID, TARGET_COMP_ID, MSG_SEQ_NUM, SENDING_TIME, CL_ORD_ID, ORDER_ID,
                            EXEC_ID,  SYMBOL,         SIDE,           EXEC_TYPE,   ORD_STATUS,   PRICE,     ORDER_QTY,
                            CUM_QTY,  LEAVES_QTY,     LAST_PX,        LAST_QTY,    TEXT};

constexpr size_t NUM_HOT_TAGS = 19;

// Helper function to check if a tag number is a hot tag
// Uses efficient switch statement for O(1) lookup
inline bool IsHotTag(int tag) {
	switch (tag) {
	case MSG_TYPE:
	case SENDER_COMP_ID:
	case TARGET_COMP_ID:
	case MSG_SEQ_NUM:
	case SENDING_TIME:
	case CL_ORD_ID:
	case ORDER_ID:
	case EXEC_ID:
	case SYMBOL:
	case SIDE:
	case EXEC_TYPE:
	case ORD_STATUS:
	case PRICE:
	case ORDER_QTY:
	case CUM_QTY:
	case LEAVES_QTY:
	case LAST_PX:
	case LAST_QTY:
	case TEXT:
		return true;
	default:
		return false;
	}
}

} // namespace FixHotTags
} // namespace duckdb
