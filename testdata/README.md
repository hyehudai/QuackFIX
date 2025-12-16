# QuackFIX Test Data

This directory contains FIX message test data for QuackFIX testing.

## File Descriptions

### sample.fix
Basic test messages without repeating groups. Used for core functionality testing.

**Contents:**
- 2 NewOrderSingle messages (MsgType=D)
- 3 ExecutionReport messages (MsgType=8)  
- 1 OrderCancelRequest message (MsgType=W)

**Features tested:**
- Hot tag parsing (19 fields)
- Non-hot tags in `tags` column
- Numeric conversions (BIGINT, DOUBLE)
- Timestamp parsing
- Multiple symbols (AAPL, MSFT, TSLA)

---

### groups.fix
Messages with **repeating groups** for testing groups column functionality.

## Message 1: ExecutionReport with NoPartyIDs (tag 453)
```
MsgType: 8 (ExecutionReport)
Symbol: AAPL
NoPartyIDs (453): 3 parties

Group Members:
  Party 1:
    - PartyID (448): PARTY1
    - PartyIDSource (447): D
    - PartyRole (452): 1

  Party 2:
    - PartyID (448): PARTY2
    - PartyIDSource (447): D
    - PartyRole (452): 3
    
  Party 3:
    - PartyID (448): PARTY3
    - PartyIDSource (447): D
    - PartyRole (452): 11
```

## Message 2: MarketDataSnapshot with NoMDEntries (tag 268)
```
MsgType: W (MarketDataSnapshotFullRefresh)
Symbol: MSFT
NoMDEntries (268): 3 entries

Group Members:
  Entry 1:
    - MDEntryType (269): 0 (Bid)
    - MDEntryPx (270): 380.00
    - MDEntrySize (271): 100
    
  Entry 2:
    - MDEntryType (269): 1 (Offer)
    - MDEntryPx (270): 380.50
    - MDEntrySize (271): 50
    
  Entry 3:
    - MDEntryType (269): 2 (Trade)
    - MDEntryPx (270): 379.75
    - MDEntrySize (271): 200
```

## Message 3: NewOrderSingle with NoPartyIDs (tag 453)
```
MsgType: D (NewOrderSingle)
Symbol: TSLA
Side: 2 (Sell)
OrderQty: 50
Price: 250.00
NoPartyIDs (453): 2 parties

Group Members:
  Party 1:
    - PartyID (448): BROKER1
    - PartyIDSource (447): D
    - PartyRole (452): 1
    
  Party 2:
    - PartyID (448): BROKER2
    - PartyIDSource (447): D
    - PartyRole (452): 3
```

## Message 4: ExecutionReport with NoPartyIDs (tag 453)
```
MsgType: 8 (ExecutionReport)
Symbol: TSLA
ExecType: F (Trade)
OrdStatus: 2 (Filled)
NoPartyIDs (453): 2 parties

Group Members:
  Party 1:
    - PartyID (448): BROKER1
    - PartyIDSource (447): D
    - PartyRole (452): 1
    
  Party 2:
    - PartyID (448): BROKER2
    - PartyIDSource (447): D
    - PartyRole (452): 3
```

## Message 5: MarketDataSnapshot with NoMDEntries (tag 268) - Complex
```
MsgType: W (MarketDataSnapshotFullRefresh)
Symbol: GOOGL
NoMDEntries (268): 5 entries (multiple bids/offers)

Group Members:
  Entry 1:
    - MDEntryType (269): 0 (Bid)
    - MDEntryPx (270): 140.00
    - MDEntrySize (271): 100
    
  Entry 2:
    - MDEntryType (269): 0 (Bid)
    - MDEntryPx (270): 139.95
    - MDEntrySize (271): 50
    
  Entry 3:
    - MDEntryType (269): 1 (Offer)
    - MDEntryPx (270): 140.10
    - MDEntrySize (271): 75
    
  Entry 4:
    - MDEntryType (269): 1 (Offer)
    - MDEntryPx (270): 140.15
    - MDEntrySize (271): 25
    
  Entry 5:
    - MDEntryType (269): 2 (Trade)
    - MDEntryPx (270): 139.90
    - MDEntrySize (271): 150
```

---

## Group Tags Reference

### NoPartyIDs (tag 453)
Repeating group for party identification (brokers, counterparties, etc.)

**Group Members:**
- 448: PartyID
- 447: PartyIDSource
- 452: PartyRole

**Common PartyRole Values:**
- 1: Executing Firm
- 3: Client ID
- 11: Order Origination Trader

### NoMDEntries (tag 268)
Repeating group for market data entries (bids, offers, trades)

**Group Members:**
- 269: MDEntryType
- 270: MDEntryPx (Price)
- 271: MDEntrySize (Size)

**MDEntryType Values:**
- 0: Bid
- 1: Offer
- 2: Trade

---

## Expected groups Column Structure

When Phase 5.5 is implemented, the `groups` column should return:

```sql
-- Message 1 (AAPL ExecutionReport)
groups[453] = [
  {448: 'PARTY1', 447: 'D', 452: '1'},
  {448: 'PARTY2', 447: 'D', 452: '3'},
  {448: 'PARTY3', 447: 'D', 452: '11'}
]

-- Message 2 (MSFT Market Data)
groups[268] = [
  {269: '0', 270: '380.00', 271: '100'},
  {269: '1', 270: '380.50', 271: '50'},
  {269: '2', 270: '379.75', 271: '200'}
]
```

## Testing Groups

### Query Examples (Phase 5.5)

```sql
-- Count messages with party groups
SELECT COUNT(*) FROM read_fix('testdata/groups.fix') 
WHERE groups[453] IS NOT NULL;
-- Expected: 3

-- Count messages with market data groups
SELECT COUNT(*) FROM read_fix('testdata/groups.fix')
WHERE groups[268] IS NOT NULL;
-- Expected: 2

-- Access first party ID
SELECT Symbol, groups[453][1][448] as FirstParty
FROM read_fix('testdata/groups.fix')
WHERE groups[453] IS NOT NULL;

-- Unnest market data entries
SELECT Symbol, 
       unnest(groups[268]) as md_entry
FROM read_fix('testdata/groups.fix')
WHERE MsgType = 'W';
```

---

## Format Notes

- Delimiter: Pipe (|) character for readability
- Standard FIX 4.4 format
- All messages include checksum (tag 10) though not validated
- SendingTime (tag 52) in format: YYYYMMDD-HH:MM:SS

## Adding New Test Data

When adding new test messages:
1. Use pipe (|) delimiter for readability
2. Include all required FIX fields (8, 9, 35, 49, 56, 34, 52, 10)
3. Document any new group types in this README
4. Update tests accordingly
