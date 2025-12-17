# QuackFIX User Guide

Comprehensive documentation for the QuackFIX DuckDB extension.

---

## Table of Contents

- [read_fix() Function](#read_fix-function)
  - [Basic Usage](#basic-usage)
  - [Parameters](#parameters)
  - [Output Schema](#output-schema)
- [Dictionary Management](#dictionary-management)
- [Performance & Optimization](#performance--optimization)
- [Auxiliary Functions](#auxiliary-functions)
- [Advanced Topics](#advanced-topics)

---

## read_fix() Function

The main table function for reading and parsing FIX protocol log files.

### Basic Usage

```sql
SELECT * FROM read_fix('path/to/file.fix');
```

**Output:**
```
┌─────────┬──────────────┬──────────────┬───────────┬─────────────────────┬───┬─────────┬─────────┬──────────────────────┬──────────────────────┬──────────────────────┬─────────────┐
│ MsgType │ SenderCompID │ TargetCompID │ MsgSeqNum │     SendingTime     │ … │ LastQty │  Text   │         tags         │        groups        │     raw_message      │ parse_error │
│ varchar │   varchar    │   varchar    │   int64   │      timestamp      │   │ double  │ varchar │ map(integer, varch…  │ map(integer, map(i…  │       varchar        │   varchar   │
├─────────┼──────────────┼──────────────┼───────────┼─────────────────────┼───┼─────────┼─────────┼──────────────────────┼──────────────────────┼──────────────────────┼─────────────┤
│ D       │ SENDER       │ TARGET       │         1 │ 2023-12-15 10:30:00 │ … │    NULL │ NULL    │ {10=000, 59=0, 40=…  │ NULL                 │ 8=FIX.4.4|9=178|35…  │ NULL        │
│ 8       │ TARGET       │ SENDER       │         2 │ 2023-12-15 10:30:01 │ … │   100.0 │ NULL    │ {10=000, 9=195, 8=…  │ NULL                 │ 8=FIX.4.4|9=195|35…  │ NULL        │
├─────────┴──────────────┴──────────────┴───────────┴─────────────────────┴───┴─────────┴─────────┴──────────────────────┴──────────────────────┴──────────────────────┴─────────────┤
│ 2 rows                                                                                                                                                       23 columns (11 shown) │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Parameters

#### file_path (required)
**Type:** `VARCHAR`  
**Description:** Path to FIX log file(s). Supports:
- Single file: `'logs/trading.fix'`
- Glob patterns: `'logs/*.fix'` or `'logs/2023-*.fix'`
- Cloud storage: `'s3://bucket/logs/*.fix'` (requires appropriate DuckDB extension)
- HTTP/HTTPS: `'https://example.com/data.fix'` (requires httpfs extension)

**Examples:**
```sql
-- Single file
SELECT * FROM read_fix('logs/trading.fix');
```

**Output:**
| MsgType | Symbol | Side | Price | OrderQty |
|---------|--------|------|--------|----------|
| D | AAPL | 1 | 150.50 | 100.0 |
| 8 | AAPL | 1 | 150.50 | 100.0 |
| D | MSFT | 2 | 380.25 | 50.0 |

```sql
-- Multiple files with glob
SELECT * FROM read_fix('logs/2023-12-*.fix');
```

**Output:**
| MsgType | Symbol | SendingTime |
|---------|--------|----------------------|
| D | AAPL | 2023-12-15 10:30:00 |
| 8 | AAPL | 2023-12-15 10:30:01 |
| D | MSFT | 2023-12-16 09:15:00 |
| 8 | MSFT | 2023-12-16 09:15:01 |

```sql
-- S3 bucket
SELECT * FROM read_fix('s3://my-bucket/fix-logs/*.fix');
```

**Output:**
| MsgType | Symbol | OrderQty |
|---------|--------|----------|
| D | TSLA | 200.0 |
| 8 | TSLA | 200.0 |

#### dictionary (optional)
**Type:** `VARCHAR`  
**Default:** `'dialects/FIX44.xml'`  
**Description:** Path to FIX dictionary XML file (QuickFIX format).

**Examples:**
```sql
-- Use default FIX 4.4 dictionary
SELECT * FROM read_fix('logs/trading.fix');
```

**Output:**
| MsgType | Symbol | Price |
|---------|--------|--------|
| D | AAPL | 150.50 |
| 8 | AAPL | 150.50 |

```sql
-- Use custom dictionary
SELECT * FROM read_fix('logs/trading.fix', dictionary='dialects/CME.xml');
```

**Output:**
| MsgType | Symbol | Price | CMECustomTag |
|---------|--------|--------|--------------|
| D | ES | 4500.25 | CME_VALUE |

#### delimiter (optional)
**Type:** `VARCHAR`  
**Default:** `'|'` (pipe)  
**Description:** Field delimiter character. Common values:
- `'|'` - Pipe (human-readable logs)
- `'\x01'` - SOH (standard FIX delimiter)

**Examples:**
```sql
-- Pipe-delimited (default)
SELECT * FROM read_fix('logs/trading.fix');
```

**Output:**
| MsgType | Symbol | Price |
|---------|--------|--------|
| D | AAPL | 150.50 |
| 8 | AAPL | 150.50 |

```sql
-- SOH-delimited
SELECT * FROM read_fix('logs/trading.fix', delimiter='\x01');
```

**Output:**
| MsgType | Symbol | Price |
|---------|--------|--------|
| D | MSFT | 380.25 |
| 8 | MSFT | 380.25 |

#### rtags (optional)
**Type:** `LIST(VARCHAR)`  
**Description:** Add custom tag columns by field name. Tags will be validated against the dictionary.

**Examples:**
```sql
-- Add TransactTime as a column
SELECT MsgType, Symbol, TransactTime 
FROM read_fix('logs/trading.fix', rtags=['TransactTime']);
```

**Output:**
```
┌─────────┬─────────┬───────────────────┐
│ MsgType │ Symbol  │   TransactTime    │
│ varchar │ varchar │      varchar      │
├─────────┼─────────┼───────────────────┤
│ D       │ AAPL    │ 20231215-10:30:00 │
│ 8       │ AAPL    │ NULL              │
│ D       │ MSFT    │ 20231215-10:31:00 │
└─────────┴─────────┴───────────────────┘
```

```sql
-- Add multiple custom tags
SELECT * FROM read_fix('logs/trading.fix', 
    rtags=['TransactTime', 'SecurityType', 'MaturityMonthYear']);
```

**Output:**
| MsgType | Symbol | TransactTime | SecurityType | MaturityMonthYear |
|---------|--------|----------------------|--------------|-------------------|
| D | AAPL | 20231215-10:30:00 | CS | NULL |
| 8 | AAPL | NULL | CS | NULL |
| D | ESZ3 | 20231215-10:32:00 | FUT | 202312 |

#### tagIds (optional)
**Type:** `LIST(INTEGER)`  
**Description:** Add custom tag columns by tag number. Useful for proprietary or unknown tags.

**Examples:**
```sql
-- Add tag 60 (TransactTime)
SELECT MsgType, Symbol, Tag60 
FROM read_fix('logs/trading.fix', tagIds=[60]);
```

**Output:**
| MsgType | Symbol | Tag60 |
|---------|--------|----------------------|
| D | AAPL | 20231215-10:30:00 |
| 8 | AAPL | NULL |
| D | MSFT | 20231215-10:31:00 |

```sql
-- Add multiple tags
SELECT * FROM read_fix('logs/trading.fix', tagIds=[60, 167, 200]);
```

**Output:**
| MsgType | Symbol | Tag60 | Tag167 | Tag200 |
|---------|--------|----------------------|--------|--------|
| D | AAPL | 20231215-10:30:00 | CS | NULL |
| 8 | AAPL | NULL | CS | NULL |

```sql
-- Mix with rtags (duplicates removed automatically)
SELECT * FROM read_fix('logs/trading.fix', 
    rtags=['TransactTime'], 
    tagIds=[167]);
```

**Output:**
| MsgType | Symbol | TransactTime | Tag167 |
|---------|--------|----------------------|--------|
| D | AAPL | 20231215-10:30:00 | CS |
| 8 | AAPL | NULL | CS |

### Output Schema

The `read_fix()` function returns **25 columns** (plus any custom tag columns):

#### Hot Tag Columns (19 fields)

These are the most commonly used FIX fields, parsed from every message:

| Column | Type | FIX Tag | Description |
|--------|------|---------|-------------|
| `MsgType` | VARCHAR | 35 | Message type (D, 8, W, etc.) |
| `SenderCompID` | VARCHAR | 49 | Sender company ID |
| `TargetCompID` | VARCHAR | 56 | Target company ID |
| `MsgSeqNum` | BIGINT | 34 | Message sequence number |
| `SendingTime` | TIMESTAMP | 52 | Message timestamp (UTC) |
| `ClOrdID` | VARCHAR | 11 | Client order ID |
| `OrderID` | VARCHAR | 37 | Order ID |
| `ExecID` | VARCHAR | 17 | Execution ID |
| `Symbol` | VARCHAR | 55 | Ticker symbol |
| `Side` | VARCHAR | 54 | Order side (1=Buy, 2=Sell) |
| `ExecType` | VARCHAR | 150 | Execution type |
| `OrdStatus` | VARCHAR | 39 | Order status |
| `Price` | DOUBLE | 44 | Order price |
| `OrderQty` | DOUBLE | 38 | Order quantity |
| `CumQty` | DOUBLE | 14 | Cumulative quantity |
| `LeavesQty` | DOUBLE | 151 | Leaves quantity |
| `LastPx` | DOUBLE | 31 | Last execution price |
| `LastQty` | DOUBLE | 32 | Last execution quantity |
| `Text` | VARCHAR | 58 | Free-form text |

#### Special Columns (6 fields)

| Column | Type | Description |
|--------|------|-------------|
| `tags` | MAP(INTEGER, VARCHAR) | All non-hot tags as key-value pairs |
| `groups` | MAP(INTEGER, LIST(MAP(INTEGER, VARCHAR))) | Repeating groups (nested structure) |
| `raw_message` | VARCHAR | Original FIX message |
| `parse_error` | VARCHAR | Parse/conversion errors (NULL if OK) |
| *Custom tags* | VARCHAR | Columns added via rtags/tagIds parameters |

#### Column Type Notes

**Numeric Types:**
- `BIGINT`: MsgSeqNum
- `DOUBLE`: Price, OrderQty, CumQty, LeavesQty, LastPx, LastQty
- Invalid numeric values become NULL with error in `parse_error` column

**Timestamp Type:**
- `SendingTime` parsed from FIX format: `YYYYMMDD-HH:MM:SS[.sss]`
- Assumes UTC timezone
- Supports millisecond precision
- Invalid timestamps become NULL with error in `parse_error` column

**MAP Types:**
- `tags`: Access with `tags[tag_number]`, e.g., `tags[60]`
- `groups`: Nested map, access with `groups[count_tag][index][field_tag]`

---

## Dictionary Management

QuackFIX uses QuickFIX-format XML dictionaries to understand FIX message structures.

### Default Dictionary

QuackFIX ships with `dialects/FIX44.xml` (FIX 4.4 standard) as the default dictionary.

```sql
-- Uses default FIX44.xml
SELECT * FROM read_fix('logs/trading.fix');
```

### Custom Dictionaries

You can provide custom dictionaries for:
- Different FIX versions (FIX 4.2, FIX 5.0, etc.)
- Venue-specific extensions (CME, ICE, etc.)
- Proprietary FIX dialects

```sql
-- Use FIX 5.0 dictionary
SELECT * FROM read_fix('logs/trading.fix', dictionary='dialects/FIX50.xml');

-- Use venue-specific dictionary
SELECT * FROM read_fix('logs/cme.fix', dictionary='dialects/CME_FIX44.xml');
```

### Dictionary Format

Dictionaries must be in QuickFIX XML format with:
- **Fields**: Tag definitions with name, type, and enum values
- **Messages**: Message type definitions with required/optional fields
- **Groups**: Repeating group definitions with member fields

Example dictionary structure:
```xml
<fix>
  <fields>
    <field number="55" name="Symbol" type="STRING" />
    <field number="54" name="Side" type="CHAR">
      <value enum="1" description="BUY" />
      <value enum="2" description="SELL" />
    </field>
  </fields>
  
  <messages>
    <message name="NewOrderSingle" msgtype="D" msgcat="app">
      <field name="ClOrdID" required="Y" />
      <field name="Symbol" required="Y" />
      <group name="NoPartyIDs" required="N">
        <field name="PartyID" required="N" />
        <field name="PartyRole" required="N" />
      </group>
    </message>
  </messages>
</fix>
```

### Dictionary Validation

- Tag names in `rtags` parameter are validated against the dictionary at bind time
- Unknown tags in `tagIds` are allowed (named "TagXX" where XX is the tag number)
- Groups are parsed only for message types defined in the dictionary

---

## Performance & Optimization

### Projection Pushdown

QuackFIX supports **projection pushdown** - it only processes columns that you actually request.

**Best Practice:** Select only the columns you need:

```sql
-- ❌ Slower: Processes all 25 columns
SELECT * FROM read_fix('logs/huge.fix');

-- ✅ Faster: Only processes requested columns
SELECT MsgType, Symbol, Price, OrderQty 
FROM read_fix('logs/huge.fix');
```

### Groups Column Cost

The `groups` column has significant parsing overhead (**~20-40% slower**) because it requires:
- Dictionary lookups for each message type
- Ordered tag traversal
- Nested structure construction

**Best Practice:** Omit `groups` column when not needed:

```sql
-- ❌ Slower: Groups parsed even if not used
SELECT MsgType, Symbol FROM read_fix('logs/huge.fix');

-- ✅ Faster: Groups column not in projection, skipped entirely
SELECT MsgType, Symbol FROM read_fix('logs/huge.fix');
-- Note: DuckDB automatically optimizes this via projection pushdown
```

**When you DO need groups:**
```sql
-- Groups explicitly requested, overhead is justified
SELECT MsgType, Symbol, groups[453] as Parties
FROM read_fix('logs/trading.fix')
WHERE groups IS NOT NULL;
```

### Tags Column Cost

The `tags` MAP column has moderate overhead. Same advice applies:

```sql
-- ✅ Only request tags when needed
SELECT MsgType, Symbol, tags[60] as TransactTime
FROM read_fix('logs/trading.fix');

-- ✅ Or use rtags/tagIds for better performance
SELECT MsgType, Symbol, TransactTime
FROM read_fix('logs/trading.fix', rtags=['TransactTime']);
```

### Multi-File Processing

QuackFIX processes multiple files efficiently:

```sql
-- Glob patterns automatically parallelize across files
SELECT COUNT(*) FROM read_fix('logs/2023-*.fix');

-- DuckDB's parallel execution handles multiple files
SELECT Symbol, COUNT(*) as orders
FROM read_fix('logs/*.fix')
WHERE MsgType = 'D'
GROUP BY Symbol;
```

### Performance Tips Summary

| Strategy | Impact | When to Use |
|----------|--------|-------------|
| Select specific columns | 20-50% faster | Always |
| Avoid `groups` column | 20-40% faster | When not analyzing repeating groups |
| Use `rtags`/`tagIds` | 10-20% faster | When accessing specific non-hot tags |
| Glob patterns | Better parallelism | Large datasets across multiple files |
| Filter early in WHERE | Faster scans | When filtering is selective |

---

## Auxiliary Functions

QuackFIX provides three functions to explore FIX dictionaries without reading log files.

### fix_fields(dictionary)

Returns all field definitions from the dictionary.

**Signature:**
```sql
fix_fields(dictionary VARCHAR) → TABLE(
    tag INTEGER,
    name VARCHAR,
    type VARCHAR,
    enum_values LIST(STRUCT(enum VARCHAR, description VARCHAR))
)
```

**Examples:**
```sql
-- All fields
SELECT * FROM fix_fields('dialects/FIX44.xml');
```

**Output:**
```
┌───────┬──────────────┬─────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  tag  │     name     │  type   │                                                                enum_values                                                                │
│ int32 │   varchar    │ varchar │                                               struct("enum" varchar, description varchar)[]                                               │
├───────┼──────────────┼─────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│     1 │ Account      │ STRING  │ NULL                                                                                                                                      │
│     2 │ AdvId        │ STRING  │ NULL                                                                                                                                      │
│     3 │ AdvRefID     │ STRING  │ NULL                                                                                                                                      │
│     4 │ AdvSide      │ CHAR    │ [{'enum': B, 'description': BUY}, {'enum': S, 'description': SELL}, {'enum': X, 'description': CROSS}, {'enum': T, 'description': TRADE}] │
│     5 │ AdvTransType │ STRING  │ [{'enum': N, 'description': NEW}, {'enum': C, 'description': CANCEL}, {'enum': R, 'description': REPLACE}]                                │
└───────┴──────────────┴─────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

```sql
-- Price fields only
SELECT * FROM fix_fields('dialects/FIX44.xml') 
WHERE type = 'PRICE';
```

**Output:**
```
┌───────┬─────────┬─────────┬───────────────────────────────────────────────┐
│  tag  │  name   │  type   │                  enum_values                  │
│ int32 │ varchar │ varchar │ struct("enum" varchar, description varchar)[] │
├───────┼─────────┼─────────┼───────────────────────────────────────────────┤
│     6 │ AvgPx   │ PRICE   │ NULL                                          │
│    31 │ LastPx  │ PRICE   │ NULL                                          │
│    44 │ Price   │ PRICE   │ NULL                                          │
│    99 │ StopPx  │ PRICE   │ NULL                                          │
└───────┴─────────┴─────────┴───────────────────────────────────────────────┘
```

```sql
-- Fields with enum values
SELECT tag, name, enum_values 
FROM fix_fields('dialects/FIX44.xml')
WHERE enum_values IS NOT NULL;
```

**Output:**
| tag | name | enum_values |
|-----|----------|-------------|
| 35 | MsgType | [{enum: "0", description: "Heartbeat"}, {enum: "D", description: "NewOrderSingle"}, ...] |
| 39 | OrdStatus | [{enum: "0", description: "New"}, {enum: "1", description: "PartiallyFilled"}, ...] |
| 54 | Side | [{enum: "1", description: "Buy"}, {enum: "2", description: "Sell"}] |

```sql
-- Search by name
SELECT * FROM fix_fields('dialects/FIX44.xml')
WHERE name LIKE '%Order%';
```

**Output:**
| tag | name | type | enum_values |
|-----|-------------|--------|-------------|
| 11 | ClOrdID | STRING | NULL |
| 37 | OrderID | STRING | NULL |
| 38 | OrderQty | QTY | NULL |
| 66 | ListID | STRING | NULL |

### fix_message_fields(dictionary)

Returns all fields used by each message type.

**Signature:**
```sql
fix_message_fields(dictionary VARCHAR) → TABLE(
    msgtype VARCHAR,
    name VARCHAR,
    category VARCHAR,
    tag INTEGER,
    field_name VARCHAR,
    required BOOLEAN,
    group_id INTEGER
)
```

**Column Descriptions:**
- `msgtype`: Message type code (D, 8, W, etc.)
- `name`: Message name (NewOrderSingle, ExecutionReport, etc.)
- `category`: Field category (required, optional, group)
- `tag`: Field tag number
- `field_name`: Field name
- `required`: Whether field is required
- `group_id`: Group count tag if field is inside a repeating group (NULL otherwise)

**Examples:**
```sql
-- All fields for NewOrderSingle
SELECT * FROM fix_message_fields('dialects/FIX44.xml') 
WHERE msgtype = 'D';
```

**Output:**
```
┌─────────┬────────────────┬──────────┬───────┬──────────────────┬──────────┬──────────┐
│ msgtype │      name      │ category │  tag  │    field_name    │ required │ group_id │
│ varchar │    varchar     │ varchar  │ int32 │     varchar      │ boolean  │  int32   │
├─────────┼────────────────┼──────────┼───────┼──────────────────┼──────────┼──────────┤
│ D       │ NewOrderSingle │ required │    11 │ ClOrdID          │ true     │     NULL │
│ D       │ NewOrderSingle │ required │    55 │ Symbol           │ true     │     NULL │
│ D       │ NewOrderSingle │ required │    65 │ SymbolSfx        │ true     │     NULL │
│ D       │ NewOrderSingle │ required │    48 │ SecurityID       │ true     │     NULL │
│ D       │ NewOrderSingle │ required │    22 │ SecurityIDSource │ true     │     NULL │
│ D       │ NewOrderSingle │ required │   460 │ Product          │ true     │     NULL │
│ D       │ NewOrderSingle │ required │   461 │ CFICode          │ true     │     NULL │
└─────────┴────────────────┴──────────┴───────┴──────────────────┴──────────┴──────────┘
```

```sql
-- Required fields only
SELECT msgtype, name, field_name 
FROM fix_message_fields('dialects/FIX44.xml')
WHERE required = true;
```

**Output:**
| msgtype | name | field_name |
|---------|----------------------|------------|
| D | NewOrderSingle | ClOrdID |
| D | NewOrderSingle | HandlInst |
| D | NewOrderSingle | Symbol |
| D | NewOrderSingle | Side |
| 8 | ExecutionReport | OrderID |
| 8 | ExecutionReport | ExecID |

```sql
-- Group fields
SELECT msgtype, name, field_name, group_id
FROM fix_message_fields('dialects/FIX44.xml')
WHERE group_id IS NOT NULL;
```

**Output:**
| msgtype | name | field_name | group_id |
|---------|----------------------|------------|----------|
| D | NewOrderSingle | PartyID | 453 |
| D | NewOrderSingle | PartyRole | 453 |
| 8 | ExecutionReport | PartyID | 453 |
| W | MarketDataSnapshot | MDEntryType | 268 |
| W | MarketDataSnapshot | MDEntryPx | 268 |

```sql
-- Count fields per message type
SELECT msgtype, name, COUNT(*) as field_count
FROM fix_message_fields('dialects/FIX44.xml')
GROUP BY msgtype, name
ORDER BY field_count DESC;
```

**Output:**
| msgtype | name | field_count |
|---------|----------------------|-------------|
| 8 | ExecutionReport | 87 |
| D | NewOrderSingle | 64 |
| W | MarketDataSnapshot | 45 |
| G | OrderCancelRequest | 32 |

### fix_groups(dictionary)

Returns all repeating group definitions.

**Signature:**
```sql
fix_groups(dictionary VARCHAR) → TABLE(
    group_tag INTEGER,
    field_tag LIST(INTEGER),
    message_types LIST(VARCHAR),
    name VARCHAR
)
```

**Column Descriptions:**
- `group_tag`: Group count tag (e.g., 453 for NoPartyIDs)
- `field_tag`: List of field tags in the group
- `message_types`: List of message types that use this group
- `name`: Group name (from field definition)

**Examples:**
```sql
-- All groups
SELECT * FROM fix_groups('dialects/FIX44.xml');
```

**Output:**
```
┌───────────┬──────────────────────────────────────┬──────────────────────────────────┬───────────────┐
│ group_tag │              field_tag               │          message_types           │     name      │
│   int32   │               int32[]                │            varchar[]             │    varchar    │
├───────────┼──────────────────────────────────────┼──────────────────────────────────┼───────────────┤
│        33 │ [58, 354, 355]                       │ [B, C]                           │ NoLinesOfText │
│        73 │ [11, 37, 198, 526, 66, 38, 799, 800] │ [AK, AS, BH, E, J, N]            │ NoOrders      │
│        78 │ [79, 661, 736, 467, 80]              │ [AB, AC, AR, AS, AT, D, G, J, P] │ NoAllocs      │
│       124 │ [17]                                 │ [AS, AX, AY, AZ, BA, BB, BG, J]  │ NoExecs       │
└───────────┴──────────────────────────────────────┴──────────────────────────────────┴───────────────┘
```

```sql
-- Groups used by multiple message types
SELECT group_tag, name, list_count(message_types) as msg_count
FROM fix_groups('dialects/FIX44.xml')
WHERE list_count(message_types) > 1
ORDER BY msg_count DESC;
```

**Output:**
| group_tag | name | msg_count |
|-----------|------------|-----------|
| 453 | NoPartyIDs | 42 |
| 232 | NoStipulations | 18 |
| 555 | NoLegs | 12 |
| 78 | NoAllocs | 8 |

```sql
-- Find groups by name
SELECT * FROM fix_groups('dialects/FIX44.xml')
WHERE name LIKE '%Party%';
```

**Output:**
| group_tag | field_tag | message_types | name |
|-----------|-----------------|---------------|------------|
| 453 | [448, 447, 452] | [D, 8, G, ...] | NoPartyIDs |

```sql
-- Groups with many fields
SELECT group_tag, name, list_count(field_tag) as field_count
FROM fix_groups('dialects/FIX44.xml')
ORDER BY field_count DESC;
```

**Output:**
| group_tag | name | field_count |
|-----------|-------------|-------------|
| 555 | NoLegs | 15 |
| 78 | NoAllocs | 12 |
| 453 | NoPartyIDs | 3 |
| 268 | NoMDEntries | 3 |

---

## Advanced Topics

### Working with Repeating Groups

Repeating groups are nested structures in FIX messages (e.g., multiple parties, market data entries).

**Structure:**
```
groups MAP(INTEGER, LIST(MAP(INTEGER, VARCHAR)))
       │            │    │
       │            │    └─ Field tag → value for each entry
       │            └────── List of group entries
       └─────────────────── Group count tag
```

**Access Patterns:**

```sql
-- Check if message has any groups
SELECT * FROM read_fix('logs/trading.fix')
WHERE groups IS NOT NULL;
```

**Output:**
| MsgType | Symbol | groups |
|---------|--------|--------|
| 8 | TSLA | {453: [{448: "BROKER1", 447: "D", 452: "1"}, {448: "CLEARHOUSE", 447: "D", 452: "4"}]} |
| W | AAPL | {268: [{269: "0", 270: "150.45", 271: "500"}, {269: "1", 270: "150.55", 271: "300"}]} |

```sql
-- Check for specific group (NoPartyIDs = tag 453)
SELECT * FROM read_fix('logs/trading.fix')
WHERE groups[453] IS NOT NULL;
```

**Output:**
| MsgType | Symbol | ClOrdID | groups |
|---------|--------|----------|--------|
| 8 | TSLA | ORDER789 | {453: [{448: "BROKER1", 447: "D", 452: "1"}, {448: "CLEARHOUSE", 447: "D", 452: "4"}]} |

```sql
-- Access entire group
SELECT MsgType, Symbol, groups[453] as Parties
FROM read_fix('logs/trading.fix');
```

**Output:**
| MsgType | Symbol | Parties |
|---------|--------|---------|
| D | AAPL | NULL |
| 8 | AAPL | NULL |
| 8 | TSLA | [{448: "BROKER1", 447: "D", 452: "1"}, {448: "CLEARHOUSE", 447: "D", 452: "4"}] |

```sql
-- Unnest groups (requires DuckDB list functions)
-- This is more complex, see DuckDB docs for unnest patterns
```

**Common Group Tags:**
- 453: NoPartyIDs (parties/brokers)
- 268: NoMDEntries (market data entries)
- 555: NoLegs (multileg instruments)
- 78: NoAllocs (allocations)

### Working with the Tags Map

The `tags` column contains all non-hot tags as a MAP:

```sql
-- Access specific tag
SELECT Symbol, tags[60] as TransactTime 
FROM read_fix('logs/trading.fix');
```

**Output:**
```
┌─────────┬───────────────────┐
│ Symbol  │   TransactTime    │
│ varchar │      varchar      │
├─────────┼───────────────────┤
│ AAPL    │ 20231215-10:30:00 │
│ AAPL    │ NULL              │
│ MSFT    │ 20231215-10:31:00 │
│ MSFT    │ NULL              │
└─────────┴───────────────────┘
```

```sql
-- Check if tag exists
SELECT * FROM read_fix('logs/trading.fix')
WHERE tags[60] IS NOT NULL;
```

**Output:**
| MsgType | Symbol | tags |
|---------|--------|------|
| D | AAPL | {60: "20231215-10:30:00", 21: "1", 40: "2", 59: "0"} |
| D | MSFT | {60: "20231215-10:31:00", 21: "1", 40: "2", 59: "0"} |

```sql
-- Count messages with specific tag
SELECT COUNT(*) FROM read_fix('logs/trading.fix')
WHERE tags[167] IS NOT NULL;  -- Tag 167 = SecurityType
```

**Output:**
| count |
|-------|
| 0 |

### Cloud Storage Support

QuackFIX supports all DuckDB file systems:

**S3:**
```sql
-- Load AWS extension
INSTALL aws;
LOAD aws;

-- Configure credentials (if needed)
SET s3_region='us-east-1';
SET s3_access_key_id='...';
SET s3_secret_access_key='...';

-- Read from S3
SELECT * FROM read_fix('s3://my-bucket/fix-logs/*.fix');
```

**HTTP/HTTPS:**
```sql
-- Load httpfs extension
INSTALL httpfs;
LOAD httpfs;

-- Read from URL
SELECT * FROM read_fix('https://example.com/data/trading.fix');
```

**Azure Blob Storage:**
```sql
INSTALL azure;
LOAD azure;

-- Configure and read
SELECT * FROM read_fix('azure://container/path/*.fix');
```

### Error Handling

Parse errors are non-fatal and reported in the `parse_error` column:

```sql
-- Find messages with errors
SELECT * FROM read_fix('logs/trading.fix')
WHERE parse_error IS NOT NULL;
```

**Output:**
| MsgType | Symbol | Price | parse_error |
|---------|--------|--------|-------------|
| D | AAPL | NULL | Invalid Price: 'INVALID' |
| 8 | MSFT | NULL | Invalid SendingTime: '20231315-10:30:00' |

```sql
-- Count error types
SELECT parse_error, COUNT(*) as count
FROM read_fix('logs/trading.fix')
WHERE parse_error IS NOT NULL
GROUP BY parse_error;
```

**Output:**
| parse_error | count |
|-------------|-------|
| Invalid Price: 'INVALID' | 3 |
| Invalid SendingTime: '20231315-10:30:00' | 1 |
| Invalid MsgSeqNum: 'abc' | 2 |

```sql
-- Valid messages only
SELECT * FROM read_fix('logs/trading.fix')
WHERE parse_error IS NULL;
```

**Output:**
| MsgType | Symbol | Price | parse_error |
|---------|--------|--------|-------------|
| D | AAPL | 150.50 | NULL |
| 8 | AAPL | 150.50 | NULL |
| D | MSFT | 380.25 | NULL |

**Common Errors:**
- `"Invalid MsgSeqNum: 'abc'"` - Non-numeric value in numeric field
- `"Invalid SendingTime: '20231315-10:30:00'"` - Invalid timestamp format
- `"Missing MsgType"` - Required tag 35 not found

### Time-Based Analysis

SendingTime is a TIMESTAMP, enabling time-based queries:

```sql
-- Filter by date
SELECT * FROM read_fix('logs/trading.fix')
WHERE SendingTime::DATE = '2023-12-15';
```

**Output:**
| MsgType | Symbol | SendingTime |
|---------|--------|----------------------|
| D | AAPL | 2023-12-15 10:30:00 |
| 8 | AAPL | 2023-12-15 10:30:01 |
| D | MSFT | 2023-12-15 10:31:00 |

```sql
-- Filter by time range
SELECT * FROM read_fix('logs/trading.fix')
WHERE SendingTime BETWEEN '2023-12-15 09:30:00' AND '2023-12-15 16:00:00';
```

**Output:**
| MsgType | Symbol | SendingTime |
|---------|--------|----------------------|
| D | AAPL | 2023-12-15 10:30:00 |
| 8 | AAPL | 2023-12-15 10:30:01 |
| D | MSFT | 2023-12-15 10:31:00 |
| 8 | MSFT | 2023-12-15 10:31:01 |

```sql
-- Extract hour
SELECT HOUR(SendingTime) as hour, COUNT(*) as messages
FROM read_fix('logs/trading.fix')
GROUP BY hour
ORDER BY hour;
```

**Output:**
| hour | messages |
|------|----------|
| 10 | 6 |
| 11 | 3 |
| 14 | 5 |

```sql
-- Time-based aggregation
SELECT 
    DATE_TRUNC('hour', SendingTime) as hour,
    Symbol,
    COUNT(*) as orders,
    SUM(OrderQty) as total_qty
FROM read_fix('logs/trading.fix')
WHERE MsgType = 'D'
GROUP BY hour, Symbol;
```

**Output:**
| hour | Symbol | orders | total_qty |
|----------------------|--------|--------|-----------|
| 2023-12-15 10:00:00 | AAPL | 1 | 100.0 |
| 2023-12-15 10:00:00 | MSFT | 1 | 50.0 |
| 2023-12-15 11:00:00 | TSLA | 2 | 400.0 |

### Combining with Other Data Sources

QuackFIX works seamlessly with other DuckDB data:

```sql
-- Join FIX data with reference data
SELECT 
    f.Symbol,
    f.OrderQty,
    f.Price,
    r.sector,
    r.market_cap
FROM read_fix('logs/trading.fix') f
JOIN read_csv('reference/securities.csv') r
    ON f.Symbol = r.symbol
WHERE f.MsgType = 'D';
```

**Output:**
| Symbol | OrderQty | Price | sector | market_cap |
|--------|----------|--------|-----------|------------|
| AAPL | 100.0 | 150.50 | Technology | 2.8T |
| MSFT | 50.0 | 380.25 | Technology | 2.5T |
| TSLA | 200.0 | 250.75 | Automotive | 800B |

```sql
-- Combine multiple data sources
SELECT 
    'FIX' as source,
    Symbol,
    OrderQty,
    Price
FROM read_fix('logs/fix/*.fix')
UNION ALL
SELECT 
    'CSV' as source,
    symbol as Symbol,
    quantity as OrderQty,
    price as Price
FROM read_csv('logs/csv/*.csv');
```

**Output:**
| source | Symbol | OrderQty | Price |
|--------|--------|----------|--------|
| FIX | AAPL | 100.0 | 150.50 |
| FIX | MSFT | 50.0 | 380.25 |
| CSV | GOOGL | 75.0 | 140.20 |
| CSV | AMZN | 125.0 | 178.50 |

---

## Quick Reference

### Functions
| Function | Purpose |
|----------|---------|
| `read_fix(path)` | Read and parse FIX log files |
| `fix_fields(dict)` | Explore field definitions |
| `fix_message_fields(dict)` | Explore message structures |
| `fix_groups(dict)` | Explore repeating groups |

### Common Patterns
```sql
-- Basic read
SELECT * FROM read_fix('logs/trading.fix');

-- Filter by message type
WHERE MsgType = 'D'

-- Access non-hot tag
tags[60] as TransactTime

-- Check for groups
WHERE groups IS NOT NULL

-- Access specific group
groups[453] as Parties

-- Time filtering
WHERE SendingTime > '2023-12-15 10:00:00'

-- Numeric operations
SUM(OrderQty), AVG(Price)
```

---

For more information, see:
- [README.md](README.md) - Quick start and examples
- [testdata/README.md](testdata/README.md) - Test data documentation

