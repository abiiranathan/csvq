# csvq 🔍

> A blazing fast, C-based command-line tool to pretty-print, filter, query, aggregate, and convert CSV/TSV data.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C11-orange.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

**csvq** is not just a pretty-printer. It is a lightweight CLI swiss-army knife for
tabular data. Inspect a messy CSV in your terminal, filter and sort millions of rows,
deduplicate records, compute grouped aggregates, convert a dataset to JSON for an API,
or generate Markdown tables for documentation — all with a single static binary and a
low memory footprint.

- [Features](#-features)
- [Installation](#-installation)
- [Quick start](#-quick-start)
- [Input handling](#-input-handling)
- [Usage](#-usage)
- [WHERE reference](#-where-reference)
- [Sorting reference](#-sorting-reference)
- [Duplicate removal](#-duplicate-removal)
- [Aggregation and grouping](#-aggregation-and-grouping)
- [Output formats](#-output-formats)
- [Command-line reference](#-command-line-reference)
- [Composition order](#-composition-order)
- [Limitations](#-limitations)
- [Testing](#-testing)
- [Contributing](#-contributing)
- [License](#-license)

## 🚀 Features

*   **Pretty printing** — auto-calculated column widths, box-drawing borders, and
    per-column ANSI colors.
*   **SQL-like filtering** — `--where` supports `=`, `!=`, `>`, `>=`, `<`, `<=`,
    `contains`, `AND`, `OR`, and parentheses.
*   **Substring search** — `--filter` performs a case-insensitive search across every
    field in a row.
*   **Sorting** — sort by one or more columns, numerically when possible and
    case-insensitively otherwise. Each sort key can have its own direction.
*   **Duplicate removal** — `--unique` removes exact duplicate rows; `--unique-by`
    deduplicates using one or more key columns.
*   **Aggregation** — `count`, `count(*)`, `sum`, `avg`, `min`, and `max`.
*   **Grouping** — `--group-by` produces one aggregate row per group, including
    multi-column groups.
*   **Quick analysis** — `--count` for row counts and `--describe` for per-column
    numeric statistics.
*   **Column management** — select and reorder columns by name or index, and hide
    columns by index.
*   **Pagination** — `--offset` and `--limit` window the result after filtering,
    sorting, and deduplication.
*   **Multi-format export** — table, CSV, TSV, JSON, Markdown, HTML, and Excel
    (XML Spreadsheet 2003).
*   **Standard input** — read CSV from a pipe with `-` or `--stdin`.
*   **Fast and efficient** — written in C11, compiled with `-O3`, no runtime
    dependencies beyond the static [SolidC](https://github.com/abiiranathan/solidc)
    library.
*   **Robust parsing** — quoted fields, custom delimiters (including tabs), custom
    comment characters, and trailing empty fields.

## 📦 Installation

### Prerequisites

*   A C11 compiler (GCC or Clang). MSVC has not been tested.
*   [SolidC](https://github.com/abiiranathan/solidc) installed so that
    `solidc/*.h` is on the include path and `libsolidc.a` is on the library path
    (the default Makefile expects `/usr/local/include` and `/usr/local/lib`).

### Building from source

```bash
git clone https://github.com/abiiranathan/csvq.git
cd csvq
make
```

The result is a single `csvq` executable in the repository root.

Additional Makefile targets:

```bash
make debug          # unoptimized build with debug symbols
make test           # build and run the integration suite
make check          # alias for make test
make windows        # cross-compile with zig cc
make macos          # cross-compile Intel and Apple Silicon binaries
make clean          # remove built binaries
```

### Project structure

```text
csvq/
├── include/
│   ├── types.h
│   └── where-parser.h
├── src/
│   ├── csvq.c
│   └── where-parser.c
├── tests/
│   └── test_csvq.sh
├── LICENSE
├── Makefile
└── README.md
```

## ⚡ Quick start

```bash
# Pretty-print a file
csvq data.csv

# Filter, sort, and show only the columns you care about
csvq employees.csv \
  --where "department = engineering AND salary >= 90000" \
  --sort=-salary \
  --select "name,salary,title"

# Convert to JSON
csvq users.csv --output json > users.json

# Count unique customers per region
csvq orders.csv --group-by region --aggregate "count,sum(amount),avg(amount)"

# Read from a pipe
cat data.csv | csvq - --unique --sort name --output markdown
```

## 📥 Input handling

The input file is the only positional argument:

```bash
csvq data.csv
```

The first row is treated as a header by default. Use `--skip-header` (or `-s`) when
the file has no header; the first row then becomes data and columns are referred to
by zero-based index.

```bash
csvq no-header.csv --skip-header --select "0,2"
```

`--header` / `-h` is the explicit form of the default (the file has a header). It is
useful when composing commands where the default may be unclear. Note that `-h` is
**not** help; use `--help` for usage information, or run `csvq` without a filename.

### Standard input

Use a bare `-` or `--stdin` to read from standard input:

```bash
cat data.csv | csvq - --count
curl -s https://example.com/data.csv | csvq --stdin --output json
```

Because the underlying CSV parser needs a seekable stream, csvq copies standard
input to a temporary file, parses it, and deletes the temporary file before exiting.

### Delimiters and comments

*   The default delimiter is `,`.
*   Use `--delimiter '\t'` for tab-separated files.
*   Any single character can be used as a delimiter: `--delimiter '|'`.
*   Lines whose first character is `#` are ignored by default.
*   Use `--comment ';'` (or another character) to change the comment marker.

```bash
csvq data.tsv --delimiter '\t'
csvq export.psv --delimiter '|'
csvq config.csv --comment ';'
```

## 📖 Usage

### Basic table view

```bash
csvq data.csv
```

The table renderer prints a box-drawing table and a `(N rows)` footer. `N` reflects
the rows actually displayed after filtering, deduplication, and pagination.

### Substring filtering

`--filter` (or `-f`) performs a case-insensitive substring search across all fields
in a row. It is not a regular expression.

```bash
# Any row containing "usb" in any column
csvq inventory.csv --filter usb

# Case-insensitive
csvq logs.csv -f ERROR
```

### WHERE filtering

`--where` (or `-w`) filters rows with a SQL-like expression:

```bash
csvq inventory.csv --where "price < 50 AND item contains USB"
csvq people.csv --where "age >= 18 AND (city = London OR city = Paris)"
```

See the [WHERE reference](#-where-reference) for operators and precedence.

### Sorting

Sort by a column name or zero-based index:

```bash
csvq employees.csv --sort salary
csvq employees.csv --sort 3 --desc
```

Multiple comma-separated keys are supported, and each key can have its own
direction:

```bash
# Region ascending, then amount descending within each region
csvq sales.csv --sort "region,-amount"

# Explicit ascending prefixes
csvq sales.csv --sort "+region,+amount"
```

`--desc` / `-D` sets the default direction for keys without a prefix:

```bash
# Both keys descending
csvq sales.csv --sort "region,amount" --desc
```

See the [sorting reference](#-sorting-reference) for details and the leading-dash
caveat.

### Column selection and hiding

`--select` (or `-S`) selects and reorders columns. It accepts names or zero-based
indices:

```bash
csvq contacts.csv --select "name,email"
csvq contacts.csv --select "0,2,1"
```

`--hide` (or `-H`) hides columns by zero-based index:

```bash
# Hide the first and fourth columns
csvq users.csv --hide "0,3"
```

Unknown names in `--select` produce a warning and are skipped. Invalid indices in
`--hide` produce a warning and are skipped.

Column references made only of digits are interpreted as zero-based indices.
Otherwise they are matched case-insensitively against the header. This rule applies
to `--select`, `--sort`, `--unique-by`, `--group-by`, and column arguments inside
`--aggregate`.

### Pagination

`--offset` / `-O` skips rows and `--limit` / `-l` caps the number of rows. Both are
applied after filtering, sorting, and deduplication:

```bash
csvq events.csv --where "status = success" --offset 1000 --limit 50
csvq people.csv --sort name --offset 20 --limit 10
```

A limit of `0` keeps only the header row. An offset beyond the result set produces
an empty result (header only for CSV-like formats).

### Quick analysis

`--count` / `-n` prints only the number of matching data rows:

```bash
csvq logs.csv --where "level = error" --count
csvq logs.csv --filter timeout -n
```

`--describe` / `-a` prints per-column statistics in a table:

| Column | Numeric | Missing | NonNumeric | Min | Max | Mean |
| ------ | ------- | ------- | ---------- | --- | --- | ---- |
| ...    | ...     | ...     | ...        | ... | ... | ...  |

*   **Numeric** — fields that parse completely as numbers.
*   **Missing** — blank or whitespace-only fields.
*   **NonNumeric** — non-blank fields that are not numbers.
*   **Min / Max / Mean** — computed over numeric values only; `-` when there are no
    numeric values.

```bash
csvq sales.csv --where "region = East" --describe
csvq people.csv --describe --select "age,score"
```

`--describe` honors `--select` and `--hide`. It prints a summary line to standard
error, for example `Describe summary computed on 5 filtered rows`.

### Duplicate removal

`--unique` / `-u` removes exact duplicate rows, comparing every column after
trimming surrounding whitespace:

```bash
csvq contacts.csv --unique
```

`--unique-by` / `-U` deduplicates using one or more key columns:

```bash
# Keep the first row for each id
csvq contacts.csv --unique-by id

# Composite key
csvq orders.csv --unique-by "customer_id,order_date"
```

The first occurrence in the current row order is kept. Filtering happens before
deduplication, so `--where` and `--unique` compose naturally. Deduplication also
affects `--count`, `--describe`, and aggregation.

If `--unique-by` refers only to unknown columns, csvq prints a warning and skips
deduplication rather than failing.

## 🔎 WHERE reference

`--where` accepts a boolean expression made of conditions, `AND`, `OR`, and
parentheses.

### Operators

| Operator   | Meaning               | Comparison                         |
| ---------- | --------------------- | ---------------------------------- |
| `=`        | Equals                | Case-insensitive string comparison |
| `!=`       | Not equals            | Case-insensitive string comparison |
| `contains` | Substring match       | Case-insensitive                   |
| `>`        | Greater than          | Numeric                            |
| `>=`       | Greater than or equal | Numeric                            |
| `<`        | Less than             | Numeric                            |
| `<=`       | Less than or equal    | Numeric                            |

*   String comparisons trim surrounding whitespace and ignore case.
*   Numeric operators require both the field and the literal to parse as complete
    numbers. Non-numeric fields do not match.
*   Column names are matched case-insensitively against the header.
*   Column names and values may contain spaces. `AND` and `OR` are recognized as
    keywords only when surrounded by spaces.
*   Values are written without quotes: `city = New York`, not `city = "New York"`.

### Precedence and grouping

`AND` binds tighter than `OR`. Parentheses override precedence.

```bash
# Equivalent to: age > 30 OR (city = London AND score > 80)
csvq people.csv --where "age > 30 OR city = London AND score > 80"

# Explicit grouping
csvq people.csv --where "(age > 30 OR city = London) AND score > 80"
```

### Error behavior

*   An unknown column produces a warning and matches no rows.
*   A malformed expression (for example a missing operator or mismatched
    parentheses) prints a diagnostic and the filter is ignored, so all rows are
    shown. csvq still exits successfully in this case.

## ↕️ Sorting reference

`--sort` / `-B` accepts a comma-separated list of up to eight keys. A key may be:

*   A column name: `--sort city`
*   A zero-based column index: `--sort 2`
*   A name or index prefixed with `-` for descending: `--sort=-salary`
*   A name or index prefixed with `+` for ascending: `--sort +name`

Keys without a prefix use the direction selected by `--desc` / `-D`.

### Comparison rules

*   If both values parse completely as numbers, they are compared numerically.
*   Otherwise they are compared as case-insensitive strings.
*   Missing columns compare as empty strings.
*   The header row is never sorted.

### The leading-dash caveat

Because the flag parser treats arguments beginning with `-` as flags, a descending
first key must be attached to the flag with `=`:

```bash
# Correct: attach the value with '='
csvq people.csv --sort=-city,age

# Wrong: the separate argument -city,age is parsed as flags
csvq people.csv --sort -city,age
```

A descending key that is not first is safe inside the comma-separated value:
`--sort "city,-age"`. The same `=` form works for the short flag:
`csvq people.csv -B=-city,age`.

Unknown sort keys print a warning and are skipped. If no key resolves, sorting is
skipped and the original order is preserved.

## 🧹 Duplicate removal

```bash
csvq data.csv --unique
csvq data.csv --unique-by id
csvq data.csv --unique-by "id,date"
```

See [Duplicate removal](#-duplicate-removal) above for ordering and interaction
details.

## 📊 Aggregation and grouping

`--aggregate` / `-A` computes summary values. `--group-by` / `-g` splits rows into
groups first. Aggregation and grouping take precedence over `--count` and
`--describe`.

### Aggregate functions

| Expression    | Result                            |
| ------------- | --------------------------------- |
| `count`       | Number of rows in the group       |
| `count(*)`    | Same as `count`                   |
| `sum(column)` | Sum of numeric values             |
| `avg(column)` | Arithmetic mean of numeric values |
| `min(column)` | Smallest numeric value            |
| `max(column)` | Largest numeric value             |

Function names are case-insensitive. Column references may be names or zero-based
indices:

```bash
csvq sales.csv --aggregate "count,sum(amount),avg(amount),min(amount),max(amount)"
csvq sales.csv --aggregate "sum(2)"
```

Multiple expressions are comma-separated. Each output column uses the expression
text as its header, so `sum(amount)` appears as the column name `sum(amount)`.

### Whole-file summary

Without `--group-by`, aggregation produces exactly one summary row:

```bash
$ csvq sales.csv --aggregate "count,sum(amount),avg(amount)" --output csv
count,sum(amount),avg(amount)
4,100,25
```

### Grouped aggregates

With `--group-by`, one row is produced per distinct combination of group columns:

```bash
$ csvq sales.csv --group-by region --aggregate "count,sum(amount),avg(qty)" --output csv
region,count,sum(amount),avg(qty)
East,2,40,3
West,2,60,4
```

Multiple group columns create composite groups:

```bash
$ csvq sales.csv --group-by "region,product" --aggregate "sum(amount)" --output csv
region,product,sum(amount)
East,Widget,10
West,Widget,20
East,Gadget,30
West,Gadget,40
```

If `--group-by` is supplied without `--aggregate`, csvq defaults to `count`:

```bash
$ csvq sales.csv --group-by region --output csv
region,count
East,2
West,2
```

### Missing and non-numeric values

For `sum`, `avg`, `min`, and `max`:

*   Blank and whitespace-only fields are ignored.
*   Non-numeric fields are ignored.
*   If a group has no numeric values, the result is `-`.
*   `count` counts rows regardless of their values.

```bash
$ csvq mixed.csv --group-by group --aggregate "count,sum(value),avg(value)" --output csv
group,count,sum(value),avg(value)
a,3,1,1
b,1,4,4
c,1,-,-
```

### Group order

Groups appear in first-appearance order after filtering, deduplication, and sorting.
If you want deterministic group ordering, combine `--sort` with `--group-by`.

### Interaction with other options

*   `--where` and `--filter` restrict the rows that are aggregated.
*   `--unique` and `--unique-by` remove duplicates before aggregation.
*   `--limit` and `--offset` window the aggregate result rows (not the input rows).
*   `--select` and `--hide` do **not** affect aggregate output; group columns and
    aggregate expressions define the output columns.
*   Aggregation output always includes a header row, even with `--skip-header`.
*   Every output format works, including JSON, Markdown, HTML, and Excel.

## 🎨 Output formats

Select a format with `--output` / `-o`:

| Format     | Aliases | Description                                |
| ---------- | ------- | ------------------------------------------ |
| `table`    | —       | Box-drawing table with a row-count footer  |
| `csv`      | —       | Comma-separated values, quoted when needed |
| `tsv`      | —       | Tab-separated values                       |
| `json`     | —       | Array of objects; all values are strings   |
| `markdown` | `md`    | GitHub-flavored Markdown table             |
| `html`     | —       | HTML `<table>` with escaped cells          |
| `excel`    | `xls`   | XML Spreadsheet 2003 workbook              |

Unknown format names print a warning and fall back to `table`.

### Table

The default. Column widths are computed from the visible data. `--color` adds ANSI
colors per column. `--bgcolor` is accepted for compatibility but currently does not
change the output.

### CSV and TSV

CSV output re-quotes fields that contain commas, quotes, or newlines. TSV output uses
tabs. Values are trimmed in the same way as the other renderers.

### JSON

```bash
csvq users.csv --output json
```

```json
[
  {"id": "1", "name": "Alice", "active": "true"},
  {"id": "2", "name": "Bob", "active": "false"}
]
```

Notes:

*   Every value is emitted as a JSON string, including numbers.
*   Strings are escaped for JSON.
*   With `--skip-header`, there are no column names, so each object uses the key
    `"field"` repeatedly.
*   An empty result renders as an empty array:
    ```json
    [
    ]
    ```

### Markdown

```bash
csvq metrics.csv --output markdown
```

When `--filter` or `--where` is active, the Markdown renderer appends a footer:

```text
Filtered: 2/5 rows matched
```

### HTML

```bash
csvq users.csv --output html
```

The output is a complete `<table>` element. Cell contents and header names are
escaped for XML/HTML. With `--skip-header`, the `<thead>` is omitted.

### Excel

```bash
csvq users.csv --output excel > users.xml
```

Excel output is an XML Spreadsheet 2003 workbook (`<?mso-application
progid="Excel.Sheet"?>`). Numeric cells use `ss:Type="Number"`; other cells use
`ss:Type="String"`. Header cells use the `sHeader` style. The file can be opened
directly in Excel, LibreOffice Calc, and similar tools.

### Colors

`--color` / `-C` colors table cells per column and is also honored by the
`--describe` renderer. Structured formats (CSV, TSV, JSON, Markdown, HTML, Excel)
never contain ANSI escape sequences.

## 🔧 Command-line reference

```text
csvq [flags] <filename>
csvq [flags] -            # read from stdin
csvq [flags] --stdin      # read from stdin
```

| Flag            | Short | Value       | Default   | Description                                                           |
| --------------- | ----- | ----------- | --------- | --------------------------------------------------------------------- |
| `--header`      | `-h`  | —           | on        | Treat the first row as a header                                       |
| `--help`        | —     | —           | —         | Print usage information and exit                                      |
| `--skip-header` | `-s`  | —           | off       | Treat the first row as data                                           |
| `--color`       | `-C`  | —           | off       | Use ANSI colors for table columns                                     |
| `--bgcolor`     | `-G`  | —           | off       | Accepted for compatibility; currently no visual effect                |
| `--desc`        | `-D`  | —           | off       | Default sort direction is descending                                  |
| `--count`       | `-n`  | —           | off       | Print the number of matching rows                                     |
| `--describe`    | `-a`  | —           | off       | Print per-column numeric statistics                                   |
| `--unique`      | `-u`  | —           | off       | Remove exact duplicate rows                                           |
| `--version`     | `-V`  | —           | off       | Print version information and exit                                    |
| `--stdin`       | —     | —           | off       | Read CSV data from standard input                                     |
| `--comment`     | `-c`  | character   | `#`       | Comment character                                                     |
| `--delimiter`   | `-d`  | string      | `,`       | Field delimiter; use `\t` for tab                                     |
| `--hide`        | `-H`  | indices     | —         | Comma-separated zero-based column indices to hide                     |
| `--filter`      | `-f`  | text        | —         | Case-insensitive substring search across all fields                   |
| `--where`       | `-w`  | expression  | —         | SQL-like row filter                                                   |
| `--select`      | `-S`  | columns     | —         | Select/reorder columns by name or index                               |
| `--output`      | `-o`  | format      | `table`   | `table`, `csv`, `tsv`, `json`, `markdown`/`md`, `html`, `excel`/`xls` |
| `--sort`        | `-B`  | columns     | —         | Sort keys; comma-separated, optional `-`/`+` prefixes                 |
| `--unique-by`   | `-U`  | columns     | —         | Deduplicate using these columns                                       |
| `--group-by`    | `-g`  | columns     | —         | Group rows before aggregating                                         |
| `--aggregate`   | `-A`  | expressions | —         | `count`, `count(*)`, `sum(col)`, `avg(col)`, `min(col)`, `max(col)`   |
| `--limit`       | `-l`  | integer     | unlimited | Maximum result rows                                                   |
| `--offset`      | `-O`  | integer     | `0`       | Skip this many result rows                                            |

Boolean flags can be combined where the flag parser allows it. Long options that
take values can use either `--flag value` or `--flag=value`. Use the `=` form when
the value itself starts with `-`.

## 🔄 Composition order

For normal rendering, the pipeline is:

1.  Parse the CSV.
2.  Sort (if requested).
3.  Apply `--filter` and `--where`.
4.  Remove duplicates (`--unique` / `--unique-by`).
5.  Apply `--offset` and `--limit`.
6.  Render.

`--count` and `--describe` are analysis modes: they apply steps 1–4 but ignore
`--offset` and `--limit`.

For aggregation, the pipeline is:

1.  Parse the CSV.
2.  Sort (if requested; this also determines group order).
3.  Apply `--filter` and `--where`.
4.  Remove duplicates.
5.  Group and aggregate.
6.  Apply `--offset` and `--limit` to the aggregate rows.
7.  Render.

Sorting happens before filtering in the implementation, but because filtering
preserves order the visible result is the same as filtering first and then sorting.

## ⚠️ Limitations

*   **Fixed limits** — up to 8 sort keys, 8 group-by columns, 16 aggregate
    expressions, 64 selected columns, and 64 hidden columns.
*   **Row length** — the bundled CSV parser uses a fixed 1024-byte line buffer
    (`MAX_FIELD_SIZE`). Rows longer than roughly 1 KB are not supported.
*   **Uniform field count** — every row must have the same number of fields as the
    first non-comment row. Ragged input fails with an `invalid number of fields`
    error.
*   **Numeric aggregation only** — `min` and `max` compare numbers, not strings.
    Non-numeric values are ignored rather than compared lexicographically.
*   **`--count` and `--describe` ignore pagination** — `--limit` and `--offset`
    affect rendered rows and aggregate result rows, not count/describe analysis.
*   **No aggregate expressions** — aggregates are limited to the six functions
    listed above. Arithmetic such as `sum(price * qty)` is not supported.
*   **Aggregate output ignores selection** — `--select` and `--hide` do not change
    aggregation output. Group columns and aggregate expressions define the columns.
*   **Group order** — groups are emitted in first-appearance order. Add `--sort` if
    you need deterministic ordering.
*   **JSON values are strings** — numbers are not emitted as JSON numbers.
*   **No header in JSON** — `--skip-header` JSON uses the repeated key `"field"`.
*   **`--filter` is not a regex** — it is a case-insensitive substring search.
*   **`--bgcolor` is a no-op** — the flag is accepted but does not currently alter
    output.
*   **Leading-dash sort keys** — use the `=` form, for example
    `--sort=-col,...` or `-B=-col,...`. A separate `-col,...` argument is consumed
    as flags.
*   **Stdin is copied to disk** — standard input is spooled to a temporary file so
    the parser can seek. Very large pipes therefore use temporary disk space.
*   **CSV dialect** — comma by default, double quotes for quoting, no configurable
    quote character, and comment lines are recognized only when the comment
    character is the first character of the line.
*   **Single-character lines** — the current SolidC parser treats a line whose
    trimmed content is exactly one character as blank. This mainly affects
    single-column files containing one-character values.
*   **Embedded quotes are not preserved** — the parser strips quote characters
    inside quoted fields, so `"he said ""hi"""` is read as `he said hi`. Quoting
    still protects delimiters and whitespace inside a field.

## 🧪 Testing

The repository ships a POSIX `sh` integration suite that exercises the real
executable end-to-end: argument parsing, CSV parsing, filtering, WHERE operators,
sorting, deduplication, aggregation, grouping, standard input, pagination, every
output format, escaping, delimiters, comments, and error handling.

```bash
make test
```

The script can also be run directly:

```bash
./tests/test_csvq.sh
CSVQ=/path/to/csvq ./tests/test_csvq.sh
```

It creates temporary fixtures, prints TAP-style `ok` / `not ok` lines, and exits
non-zero if any test fails.

## 🤝 Contributing

Contributions are what make the open-source community such an amazing place to
learn, inspire, and create. Any contributions you make are **greatly appreciated**.

1.  Fork the project.
2.  Create your feature branch: `git checkout -b feature/AmazingFeature`.
3.  Commit your changes: `git commit -m 'Add some AmazingFeature'`.
4.  Push to the branch: `git push origin feature/AmazingFeature`.
5.  Open a pull request.

Please keep the build warning-free with `-Wall -Werror -Wextra` and add integration
tests for new user-facing behavior.

### Ideas for contribution

*   Add aggregate expressions such as `sum(price * qty)`.
*   Add `stddev`, `median`, and percentile aggregates.
*   Support a configurable quote character.
*   Emit native JSON numbers when a column is entirely numeric.
*   Implement `--bgcolor` in the table renderer.

## 📜 License

Distributed under the MIT License. See [`LICENSE`](LICENSE) for more information.

## ✍️ Author

**Dr. Abiira Nathan**
*07 December 2025*
