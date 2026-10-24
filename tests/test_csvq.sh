#!/bin/sh
# Integration tests for the csvq command-line tool.
#
# Every user-facing flag and output format is exercised through the real
# executable, so the tests cover argument parsing, CSV parsing, filtering,
# sorting, column selection, and all renderers end-to-end.
#
# Usage:
#   ./tests/test_csvq.sh [path-to-csvq]
#   CSVQ=/path/to/csvq ./tests/test_csvq.sh
#
# The script is POSIX sh compatible and cleans up its temporary fixtures.

set -u

LC_ALL=C
export LC_ALL

if [ "$#" -gt 0 ]; then
    CSVQ=$1
else
    CSVQ=${CSVQ:-./csvq}
fi

# Make the executable path absolute so it can be invoked after we cd elsewhere.
case "$CSVQ" in
    /*) ;;
    *)
        if [ -x "$CSVQ" ]; then
            CSVQ=$(cd "$(dirname "$CSVQ")" && pwd)/$(basename "$CSVQ")
        elif [ -x "../csvq" ]; then
            CSVQ=$(cd .. && pwd)/csvq
        fi
        ;;
esac

if [ ! -x "$CSVQ" ]; then
    echo "error: csvq executable not found or not executable: $CSVQ" >&2
    exit 1
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/csvq-tests.XXXXXX") || {
    echo "error: could not create a temporary directory" >&2
    exit 1
}
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

# ---------------------------------------------------------------------------
# Test harness
# ---------------------------------------------------------------------------

total=0
passed=0
failed=0

out=
err=
status=0

run() {
    out=$("$CSVQ" "$@" 2>"$tmpdir/stderr")
    status=$?
    err=$(cat "$tmpdir/stderr")
}

run_stdin() {
    input=$1
    shift
    out=$(printf '%s\n' "$input" | "$CSVQ" "$@" 2>"$tmpdir/stderr")
    status=$?
    err=$(cat "$tmpdir/stderr")
}

fail() {
    failed=$((failed + 1))
    printf 'not ok %d - %s\n' "$total" "$1"
    shift

    # Print the remaining arguments as indented diagnostics when provided.
    for diagnostic in "$@"; do
        printf '%s\n' "$diagnostic" | sed 's/^/  # /'
    done
}

pass() {
    passed=$((passed + 1))
    printf 'ok %d - %s\n' "$total" "$1"
}

check_eq() {
    total=$((total + 1))
    label=$1
    expected=$2
    actual=$3

    if [ "$expected" = "$actual" ]; then
        pass "$label"
    else
        fail "$label" "expected:" "$expected" "actual:" "$actual"
    fi
}

check_status() {
    total=$((total + 1))
    label=$1
    expected=$2
    actual=$3

    if [ "$expected" -eq "$actual" ]; then
        pass "$label"
    else
        fail "$label" "expected exit status: $expected" "actual exit status: $actual" \
            "stderr: $err"
    fi
}

check_contains() {
    total=$((total + 1))
    label=$1
    haystack=$2
    needle=$3

    case "$haystack" in
        *"$needle"*) pass "$label" ;;
        *) fail "$label" "missing text: $needle" "actual:" "$haystack" ;;
    esac
}

check_not_contains() {
    total=$((total + 1))
    label=$1
    haystack=$2
    needle=$3

    case "$haystack" in
        *"$needle"*) fail "$label" "unexpected text: $needle" "actual:" "$haystack" ;;
        *) pass "$label" ;;
    esac
}

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

people=$tmpdir/people.csv
cat >"$people" <<'EOF'
id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,
EOF

noheader=$tmpdir/noheader.csv
cat >"$noheader" <<'EOF'
1,Alice,30
2,Bob,25
EOF

numbers=$tmpdir/numbers.csv
cat >"$numbers" <<'EOF'
name,value
two,2
ten,10
one,1
EOF

pipe_file=$tmpdir/pipe.txt
cat >"$pipe_file" <<'EOF'
a|b|c
1|2|3
EOF

tab_file=$tmpdir/tab.tsv
printf 'a\tb\n1\t2\n' >"$tab_file"

comments=$tmpdir/comments.csv
cat >"$comments" <<'EOF'
# a leading comment
a,b
1,2
# another comment
3,4
EOF

semicolon_comments=$tmpdir/semicolon_comments.csv
cat >"$semicolon_comments" <<'EOF'
; ignored because --comment is ';'
a,b
1,2
; skipped too
3,4
#notcomment,5
EOF

quoted=$tmpdir/quoted.csv
cat >"$quoted" <<'EOF'
id,text
1,"hello, world"
2,plain
EOF

escaped=$tmpdir/escaped.csv
cat >"$escaped" <<'EOF'
id,text
1,back\slash
2,tab	here
3,  padded  
EOF

xml_chars=$tmpdir/xml_chars.csv
cat >"$xml_chars" <<'EOF'
id,text
1,"<a> & <b> 'x'"
EOF

ragged=$tmpdir/ragged.csv
cat >"$ragged" <<'EOF'
a,b,c
1,2
EOF

sales=$tmpdir/sales.csv
cat >"$sales" <<'EOF'
region,product,amount,qty
East,Widget,10,2
West,Widget,20,3
East,Gadget,30,4
West,Gadget,40,5
EOF

duplicates=$tmpdir/duplicates.csv
cat >"$duplicates" <<'EOF'
id,name,dept
1,Alice,eng
1,Alicia,eng
2,Bob,sales
3,Carol,eng
3,Carol,eng
EOF

mixed=$tmpdir/mixed.csv
cat >"$mixed" <<'EOF'
group,value
a,1
a,
a,nope
b,4
c,text
EOF

# ---------------------------------------------------------------------------
# Default table output
# ---------------------------------------------------------------------------

expected_table='┌────┬───────┬─────┬──────────┬───────┐
│ id │ name  │ age │ city     │ score │
├────┼───────┼─────┼──────────┼───────┤
│ 1  │ Alice │ 30  │ New York │ 95.5  │
│ 2  │ Bob   │ 25  │ London   │ 88    │
│ 3  │ Carol │ 35  │ Paris    │ 72.25 │
│ 4  │ dave  │ 28  │ new york │ 100   │
│ 5  │ Eve   │ 40  │ Berlin   │       │
└────┴───────┴─────┴──────────┴───────┘
(5 rows)'

run "$people"
check_status "default table exits successfully" 0 "$status"
check_eq "default table renders box table with row count" "$expected_table" "$out"
check_eq "default table writes nothing to stderr" "" "$err"

run "$people" --output table
check_status "explicit --output table exits successfully" 0 "$status"
check_eq "explicit --output table matches default output" "$expected_table" "$out"

run "$people" --header
check_status "explicit --header exits successfully" 0 "$status"
check_eq "explicit --header matches default output" "$expected_table" "$out"

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------

esc=$(printf '\033')
run "$people" --color
check_status "--color exits successfully" 0 "$status"
check_contains "--color emits ANSI column colors" "$out" "${esc}[36m"
check_contains "--color resets ANSI attributes" "$out" "${esc}[0m"

# --bgcolor is accepted; the current renderer does not alter output for it.
run "$people" --bgcolor
check_status "--bgcolor exits successfully" 0 "$status"
check_eq "--bgcolor preserves table output" "$expected_table" "$out"

# ---------------------------------------------------------------------------
# Header handling
# ---------------------------------------------------------------------------

expected_noheader_table='┌───┬───────┬────┐
│ 1 │ Alice │ 30 │
│ 2 │ Bob   │ 25 │
└───┴───────┴────┘
(2 rows)'

run "$noheader" --skip-header
check_status "--skip-header table exits successfully" 0 "$status"
check_eq "--skip-header treats the first row as data" "$expected_noheader_table" "$out"

run "$noheader" -s -o csv
check_status "--skip-header CSV exits successfully" 0 "$status"
check_eq "--skip-header CSV includes the first row" '1,Alice,30
2,Bob,25' "$out"

run "$noheader" -s -o json
check_status "--skip-header JSON exits successfully" 0 "$status"
check_eq "--skip-header JSON uses generic field keys" '[
  {"field": "1", "field": "Alice", "field": "30"},
  {"field": "2", "field": "Bob", "field": "25"}
]' "$out"

run "$noheader" -s -o markdown
check_eq "--skip-header markdown omits a header row" '| 1 | Alice | 30 |
| 2 | Bob | 25 |' "$out"

run "$noheader" -s -o html
check_eq "--skip-header HTML omits the thead" '<table>
  <tbody>
<tr><td>1</td><td>Alice</td><td>30</td></tr>
<tr><td>2</td><td>Bob</td><td>25</td></tr>
  </tbody>
</table>' "$out"

run "$noheader" -s -o excel
check_status "--skip-header Excel exits successfully" 0 "$status"
check_contains "--skip-header Excel emits a worksheet" "$out" '<Worksheet ss:Name="Sheet1">'
check_contains "--skip-header Excel includes the first row" "$out" '<Data ss:Type="Number">1</Data>'
check_contains "--skip-header Excel closes the workbook" "$out" '</Workbook>'

run "$noheader" -s -a
check_status "--skip-header describe exits successfully" 0 "$status"
check_contains "describe labels unheaded columns as col_0" "$out" 'col_0'
check_contains "describe labels unheaded columns as col_1" "$out" 'col_1'
check_contains "describe labels unheaded columns as col_2" "$out" 'col_2'
check_contains "describe counts the skipped-header rows" "$err" '2 filtered rows'

# ---------------------------------------------------------------------------
# Column selection and hiding
# ---------------------------------------------------------------------------

run "$people" -S 'name,id' -o csv
check_status "-S by name exits successfully" 0 "$status"
check_eq "-S by name selects and reorders columns" 'name,id
Alice,1
Bob,2
Carol,3
dave,4
Eve,5' "$out"

run "$people" --select '2,0' --output csv
check_eq "--select by index selects and reorders columns" 'age,id
30,1
25,2
35,3
28,4
40,5' "$out"

run "$people" -S 'missing,id' -o csv
check_status "-S with an unknown column still succeeds" 0 "$status"
check_contains "-S warns about an unknown column" "$err" "Column 'missing' not found"
check_eq "-S skips unknown columns" 'id
1
2
3
4
5' "$out"

run "$people" -H '1,3' -o csv
check_status "-H exits successfully" 0 "$status"
check_eq "-H hides the requested columns" 'id,age,score
1,30,95.5
2,25,88
3,35,72.25
4,28,100
5,40,' "$out"

run "$people" --hide 'bogus,1' --output csv
check_status "--hide with an invalid index still succeeds" 0 "$status"
check_contains "--hide warns about an invalid index" "$err" "Invalid column index 'bogus'"
check_eq "--hide applies valid indices and skips invalid ones" 'id,age,city,score
1,30,New York,95.5
2,25,London,88
3,35,Paris,72.25
4,28,new york,100
5,40,Berlin,' "$out"

# ---------------------------------------------------------------------------
# Substring filtering
# ---------------------------------------------------------------------------

expected_filter='id,name,age,city,score
2,Bob,25,London,88'

run "$people" --filter LONDON -o csv
check_status "--filter exits successfully" 0 "$status"
check_eq "--filter is case-insensitive and matches any field" "$expected_filter" "$out"

run "$people" -f 'new york' -o csv
check_eq "-f matches multiple rows" 'id,name,age,city,score
1,Alice,30,New York,95.5
4,dave,28,new york,100' "$out"

# ---------------------------------------------------------------------------
# WHERE clauses
# ---------------------------------------------------------------------------

run "$people" -w 'age > 25' -o csv
check_status "WHERE > exits successfully" 0 "$status"
check_eq "WHERE > filters numerically" 'id,name,age,city,score
1,Alice,30,New York,95.5
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

run "$people" --where 'age < 30' --output csv
check_eq "WHERE < filters numerically" 'id,name,age,city,score
2,Bob,25,London,88
4,dave,28,new york,100' "$out"

run "$people" -w 'age >= 35 AND age <= 40' -o csv
check_eq "WHERE >= and <= filter inclusively" 'id,name,age,city,score
3,Carol,35,Paris,72.25
5,Eve,40,Berlin,' "$out"

run "$people" -w 'city = new york' -o csv
check_eq "WHERE = compares case-insensitively" 'id,name,age,city,score
1,Alice,30,New York,95.5
4,dave,28,new york,100' "$out"

run "$people" -w 'city != new york' -o csv
check_eq "WHERE != compares case-insensitively" 'id,name,age,city,score
2,Bob,25,London,88
3,Carol,35,Paris,72.25
5,Eve,40,Berlin,' "$out"

run "$people" -w 'name contains A' -o csv
check_eq "WHERE contains is case-insensitive" 'id,name,age,city,score
1,Alice,30,New York,95.5
3,Carol,35,Paris,72.25
4,dave,28,new york,100' "$out"

run "$people" -w 'name CONTAINS A' -o csv
check_eq "WHERE operators are case-insensitive" 'id,name,age,city,score
1,Alice,30,New York,95.5
3,Carol,35,Paris,72.25
4,dave,28,new york,100' "$out"

run "$people" -w 'age > 30 OR city = London AND score > 80' -o csv
check_eq "WHERE AND binds tighter than OR" 'id,name,age,city,score
2,Bob,25,London,88
3,Carol,35,Paris,72.25
5,Eve,40,Berlin,' "$out"

run "$people" -w '(age > 30 OR city = London) AND score > 80' -o csv
check_eq "WHERE parentheses override precedence" 'id,name,age,city,score
2,Bob,25,London,88' "$out"

run "$people" -w 'age > 30 OR (city = London AND score > 80)' -o csv
check_eq "WHERE supports nested grouping" 'id,name,age,city,score
2,Bob,25,London,88
3,Carol,35,Paris,72.25
5,Eve,40,Berlin,' "$out"

# Invalid WHERE clauses are reported, but csvq falls back to showing all rows.
run "$people" -w 'age 30' -o csv
check_status "invalid WHERE without an operator exits successfully" 0 "$status"
check_contains "invalid WHERE reports the missing operator" "$err" "No valid operator"
check_eq "invalid WHERE falls back to unfiltered output" 'id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

run "$people" -w '(age > 30' -o csv
check_status "unbalanced WHERE parentheses exit successfully" 0 "$status"
check_contains "unbalanced WHERE parentheses are reported" "$err" 'Mismatched parentheses'
check_eq "unbalanced WHERE parentheses fall back to unfiltered output" 'id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

run "$people" -w 'missing = value' -o csv
check_status "WHERE on an unknown column exits successfully" 0 "$status"
check_contains "WHERE warns about an unknown column" "$err" "Column 'missing' in where clause not found"
check_eq "WHERE on an unknown column matches no rows" 'id,name,age,city,score' "$out"

run "$people" -w 'name > 10' -o csv
check_eq "numeric WHERE rejects non-numeric fields" 'id,name,age,city,score' "$out"

# ---------------------------------------------------------------------------
# Sorting
# ---------------------------------------------------------------------------

run "$people" -B age -o csv
check_status "-B exits successfully" 0 "$status"
check_eq "-B sorts numerically by column name" 'id,name,age,city,score
2,Bob,25,London,88
4,dave,28,new york,100
1,Alice,30,New York,95.5
3,Carol,35,Paris,72.25
5,Eve,40,Berlin,' "$out"

run "$people" -B age -D -o csv
check_eq "-D sorts in descending order" 'id,name,age,city,score
5,Eve,40,Berlin,
3,Carol,35,Paris,72.25
1,Alice,30,New York,95.5
4,dave,28,new york,100
2,Bob,25,London,88' "$out"

run "$people" --sort name --output csv
check_eq "--sort sorts strings case-insensitively" 'id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

run "$people" -B 0 -o csv
check_eq "-B accepts a numeric column index" 'id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

run "$numbers" -B value -o csv
check_eq "-B compares numbers rather than strings" 'name,value
one,1
two,2
ten,10' "$out"

run "$people" -B nope -o csv
check_status "sorting by an unknown column exits successfully" 0 "$status"
check_contains "sorting warns about an unknown column" "$err" "Could not resolve sort column 'nope'"
check_eq "sorting by an unknown column leaves row order unchanged" 'id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,' "$out"

# ---------------------------------------------------------------------------
# Pagination
# ---------------------------------------------------------------------------

run "$people" --offset 1 --limit 2 -o csv
check_status "--offset/--limit exit successfully" 0 "$status"
check_eq "--offset/--limit return the requested window" 'id,name,age,city,score
2,Bob,25,London,88
3,Carol,35,Paris,72.25' "$out"

run "$people" -O 1 -l 2 -o csv
check_eq "-O/-l are the short forms of offset/limit" 'id,name,age,city,score
2,Bob,25,London,88
3,Carol,35,Paris,72.25' "$out"

run "$people" -l 0 -o csv
check_status "--limit 0 exits successfully" 0 "$status"
check_eq "--limit 0 keeps only the header" 'id,name,age,city,score' "$out"

run "$people" -O 999 -o csv
check_eq "--offset beyond the row count yields no rows" 'id,name,age,city,score' "$out"

run "$people" -w 'age > 25' -B age -D -O 1 -l 2 -o csv
check_eq "filters, sorting, and pagination compose in order" 'id,name,age,city,score
3,Carol,35,Paris,72.25
1,Alice,30,New York,95.5' "$out"

# ---------------------------------------------------------------------------
# Count and describe
# ---------------------------------------------------------------------------

run "$people" --count
check_status "--count exits successfully" 0 "$status"
check_eq "--count prints the total data row count" '5' "$out"

run "$people" -n -w 'age > 25'
check_eq "-n respects WHERE filters" '4' "$out"

run "$people" -n -f london
check_eq "-n respects substring filters" '1' "$out"

expected_describe='┌────────┬─────────┬─────────┬────────────┬───────┬─────┬─────────┐
│ Column │ Numeric │ Missing │ NonNumeric │ Min   │ Max │ Mean    │
├────────┼─────────┼─────────┼────────────┼───────┼─────┼─────────┤
│ id     │ 5       │ 0       │ 0          │ 1     │ 5   │ 3       │
│ name   │ 0       │ 0       │ 5          │ -     │ -   │ -       │
│ age    │ 5       │ 0       │ 0          │ 25    │ 40  │ 31.6    │
│ city   │ 0       │ 0       │ 5          │ -     │ -   │ -       │
│ score  │ 4       │ 1       │ 0          │ 72.25 │ 100 │ 88.9375 │
└────────┴─────────┴─────────┴────────────┴───────┴─────┴─────────┘'

run "$people" --describe
check_status "--describe exits successfully" 0 "$status"
check_eq "--describe reports per-column statistics" "$expected_describe" "$out"
check_contains "--describe reports the filtered row count on stderr" "$err" \
    'Describe summary computed on 5 filtered rows'

run "$people" -a -w 'age > 25'
check_status "--describe with a filter exits successfully" 0 "$status"
check_contains "--describe applies WHERE filters" "$err" 'Describe summary computed on 4 filtered rows'
check_contains "--describe still includes every column" "$out" 'name'

run "$people" -a -S 'age,score'
check_status "--describe with --select exits successfully" 0 "$status"
check_contains "--describe honours the selected columns" "$out" 'age'
check_contains "--describe honours the selected columns (2)" "$out" 'score'
check_not_contains "--describe hides unselected columns" "$out" 'name'

run "$people" -n -l 2
check_eq "--count ignores pagination" '5' "$out"

run "$people" -a -l 1
check_contains "--describe ignores pagination" "$err" 'Describe summary computed on 5 filtered rows'

# ---------------------------------------------------------------------------
# Output formats
# ---------------------------------------------------------------------------

expected_csv='id,name,age,city,score
1,Alice,30,New York,95.5
2,Bob,25,London,88
3,Carol,35,Paris,72.25
4,dave,28,new york,100
5,Eve,40,Berlin,'

run "$people" -o csv
check_status "-o csv exits successfully" 0 "$status"
check_eq "-o csv emits CSV" "$expected_csv" "$out"

run "$people" --output csv
check_eq "--output csv emits CSV" "$expected_csv" "$out"

expected_tsv=$(printf 'id\tname\tage\tcity\tscore\n1\tAlice\t30\tNew York\t95.5\n2\tBob\t25\tLondon\t88\n3\tCarol\t35\tParis\t72.25\n4\tdave\t28\tnew york\t100\n5\tEve\t40\tBerlin\t')
run "$people" -o tsv
check_status "-o tsv exits successfully" 0 "$status"
check_eq "-o tsv emits tab-separated values" "$expected_tsv" "$out"

expected_json='[
  {"id": "1", "name": "Alice", "age": "30", "city": "New York", "score": "95.5"},
  {"id": "2", "name": "Bob", "age": "25", "city": "London", "score": "88"},
  {"id": "3", "name": "Carol", "age": "35", "city": "Paris", "score": "72.25"},
  {"id": "4", "name": "dave", "age": "28", "city": "new york", "score": "100"},
  {"id": "5", "name": "Eve", "age": "40", "city": "Berlin", "score": ""}
]'

run "$people" -o json
check_status "-o json exits successfully" 0 "$status"
check_eq "-o json emits an array of objects" "$expected_json" "$out"

expected_markdown='| id | name | age | city | score |
| --- | --- | --- | --- | --- |
| 1 | Alice | 30 | New York | 95.5 |
| 2 | Bob | 25 | London | 88 |
| 3 | Carol | 35 | Paris | 72.25 |
| 4 | dave | 28 | new york | 100 |
| 5 | Eve | 40 | Berlin |  |'

run "$people" -o markdown
check_status "-o markdown exits successfully" 0 "$status"
check_eq "-o markdown emits a GitHub table" "$expected_markdown" "$out"

run "$people" --output md
check_eq "--output md is an alias for markdown" "$expected_markdown" "$out"

expected_html='<table>
  <thead>
    <tr><th>id</th><th>name</th><th>age</th><th>city</th><th>score</th></tr>
  </thead>
  <tbody>
<tr><td>1</td><td>Alice</td><td>30</td><td>New York</td><td>95.5</td></tr>
<tr><td>2</td><td>Bob</td><td>25</td><td>London</td><td>88</td></tr>
<tr><td>3</td><td>Carol</td><td>35</td><td>Paris</td><td>72.25</td></tr>
<tr><td>4</td><td>dave</td><td>28</td><td>new york</td><td>100</td></tr>
<tr><td>5</td><td>Eve</td><td>40</td><td>Berlin</td><td></td></tr>
  </tbody>
</table>'

run "$people" -o html
check_status "-o html exits successfully" 0 "$status"
check_eq "-o html emits an HTML table" "$expected_html" "$out"

run "$people" -o excel
check_status "-o excel exits successfully" 0 "$status"
check_contains "-o excel emits the XML spreadsheet prologue" "$out" \
    '<?mso-application progid="Excel.Sheet"?>'
check_contains "-o excel writes numeric cells" "$out" '<Data ss:Type="Number">95.5</Data>'
check_contains "-o excel writes string cells" "$out" '<Data ss:Type="String">Alice</Data>'
check_contains "-o excel writes the header style" "$out" 'ss:StyleID="sHeader"'
check_contains "-o excel closes the workbook" "$out" '</Workbook>'

run "$people" --output xls
check_status "--output xls is accepted" 0 "$status"
check_contains "--output xls is an alias for excel" "$out" 'Excel.Sheet'

run "$people" -o yaml
check_status "unknown output formats still exit successfully" 0 "$status"
check_contains "unknown output formats produce a warning" "$err" "Unknown format 'yaml'"
check_eq "unknown output formats fall back to the table renderer" "$expected_table" "$out"

run "$people" -w 'age > 100' -o csv
check_eq "CSV output with no matching rows keeps the header" 'id,name,age,city,score' "$out"

run "$people" -w 'age > 100' -o json
check_eq "JSON output with no matching rows is an empty array" '[
]' "$out"

# ---------------------------------------------------------------------------
# Escaping and sanitization
# ---------------------------------------------------------------------------

run "$quoted" -o csv
check_eq "CSV output quotes fields containing commas" 'id,text
1,"hello, world"
2,plain' "$out"

run "$escaped" -o json
check_eq "JSON output escapes backslashes, tabs, and trims values" '[
  {"id": "1", "text": "back\\slash"},
  {"id": "2", "text": "tab\there"},
  {"id": "3", "text": "padded"}
]' "$out"

run "$xml_chars" -o html
check_eq "HTML output escapes XML special characters" '<table>
  <thead>
    <tr><th>id</th><th>text</th></tr>
  </thead>
  <tbody>
<tr><td>1</td><td>&lt;a&gt; &amp; &lt;b&gt; &apos;x&apos;</td></tr>
  </tbody>
</table>' "$out"

run "$xml_chars" -o excel
check_contains "Excel output escapes XML special characters" "$out" \
    '&lt;a&gt; &amp; &lt;b&gt; &apos;x&apos;'

run "$escaped"
check_status "table output with control characters exits successfully" 0 "$status"
check_contains "table output replaces tabs in cells with spaces" "$out" 'tab here'
check_not_contains "table output does not emit raw tabs inside cells" "$out" 'tab	here'

# ---------------------------------------------------------------------------
# Delimiters and comments
# ---------------------------------------------------------------------------

run "$pipe_file" -d '|' -o csv
check_status "custom delimiter exits successfully" 0 "$status"
check_eq "custom delimiter parses and emits comma-separated output" 'a,b,c
1,2,3' "$out"

run "$tab_file" -d '\t' -o csv
check_status "tab delimiter exits successfully" 0 "$status"
check_eq "tab delimiter parses TSV" 'a,b
1,2' "$out"

run "$comments" -o csv
check_status "comment handling exits successfully" 0 "$status"
check_eq "hash-prefixed lines are ignored by default" 'a,b
1,2
3,4' "$out"

run "$semicolon_comments" -c ';' -o csv
check_status "custom comment character exits successfully" 0 "$status"
check_eq "custom comment character skips semicolon lines only" 'a,b
1,2
3,4
#notcomment,5' "$out"

# ---------------------------------------------------------------------------
# Markdown filter footer
# ---------------------------------------------------------------------------

run "$people" -o markdown -w 'age > 30'
check_status "filtered markdown exits successfully" 0 "$status"
check_eq "markdown reports how many rows matched" '| id | name | age | city | score |
| --- | --- | --- | --- | --- |
| 3 | Carol | 35 | Paris | 72.25 |
| 5 | Eve | 40 | Berlin |  |

Filtered: 2/5 rows matched' "$out"

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------

run --version
check_status "--version exits successfully" 0 "$status"
check_eq "--version reports the program version" 'csvq 0.2.0' "$out"

run -V
check_status "-V exits successfully" 0 "$status"
check_eq "-V is the short form of --version" 'csvq 0.2.0' "$out"

run --help
check_status "--help exits successfully" 0 "$status"
check_contains "--help prints usage information" "$out" 'Usage:'

run --bogus
check_status "unknown flags fail" 1 "$status"
check_contains "unknown flags are reported" "$err" 'Unknown flag: --bogus'

# ---------------------------------------------------------------------------
# Standard input
# ---------------------------------------------------------------------------

run_stdin 'a,b
1,2' - -o csv
check_status "stdin via '-' exits successfully" 0 "$status"
check_eq "stdin via '-' is parsed as CSV" 'a,b
1,2' "$out"

run_stdin 'a,b
1,2' --stdin -o csv
check_status "--stdin without a filename exits successfully" 0 "$status"
check_eq "--stdin is equivalent to '-'" 'a,b
1,2' "$out"

run_stdin 'a,b
1,2
3,4' - -n
check_eq "--count works on stdin" '2' "$out"

run_stdin 'a,b
1,x
1,x
2,y' - --unique -o csv
check_eq "--unique works on stdin" 'a,b
1,x
2,y' "$out"

run_stdin 'a,b
1,2
3,4' - -A 'sum(b)' -o csv
check_eq "aggregation works on stdin" 'sum(b)
6' "$out"

run_stdin 'g,v
a,1
a,2
b,3' --stdin -g g -A 'count,sum(v)' -o csv
check_eq "grouping works on stdin" 'g,count,sum(v)
a,2,3
b,1,3' "$out"

run_stdin '1,2
3,4' - -s -o csv
check_eq "--skip-header works on stdin" '1,2
3,4' "$out"

# ---------------------------------------------------------------------------
# Duplicate removal
# ---------------------------------------------------------------------------

run "$duplicates" --unique -o csv
check_status "--unique exits successfully" 0 "$status"
check_eq "--unique removes exact duplicate rows" 'id,name,dept
1,Alice,eng
1,Alicia,eng
2,Bob,sales
3,Carol,eng' "$out"

run "$duplicates" -u -o csv
check_eq "-u is the short form of --unique" 'id,name,dept
1,Alice,eng
1,Alicia,eng
2,Bob,sales
3,Carol,eng' "$out"

run "$duplicates" --unique-by id -o csv
check_status "--unique-by exits successfully" 0 "$status"
check_eq "--unique-by keeps the first row per key" 'id,name,dept
1,Alice,eng
2,Bob,sales
3,Carol,eng' "$out"

run "$duplicates" -U dept -o csv
check_eq "-U deduplicates using the selected columns" 'id,name,dept
1,Alice,eng
2,Bob,sales' "$out"

run "$duplicates" -u -n
check_eq "--count reflects deduplicated rows" '4' "$out"

run "$duplicates" -u -a
check_status "--describe with --unique exits successfully" 0 "$status"
check_contains "--describe counts deduplicated rows" "$err" 'Describe summary computed on 4 filtered rows'

run "$duplicates" -u -w 'dept = eng' -o csv
check_eq "WHERE filtering runs before deduplication" 'id,name,dept
1,Alice,eng
1,Alicia,eng
3,Carol,eng' "$out"

run "$duplicates" -U nope -o csv
check_status "--unique-by with an unknown column still succeeds" 0 "$status"
check_contains "--unique-by warns about the unknown column" "$err" "Column 'nope' not found"
check_contains "--unique-by warns that deduplication was skipped" "$err" \
    '--unique-by did not resolve any columns'
check_contains "--unique-by with an unknown column leaves duplicates intact" "$out" \
    '3,Carol,eng
3,Carol,eng'

# ---------------------------------------------------------------------------
# Multi-column sorting
# ---------------------------------------------------------------------------

run "$sales" -B 'region,-amount' -o csv
check_status "multi-column --sort exits successfully" 0 "$status"
check_eq "multi-column --sort honours per-key direction" 'region,product,amount,qty
East,Gadget,30,4
East,Widget,10,2
West,Gadget,40,5
West,Widget,20,3' "$out"

run "$sales" --sort '+region,+amount' -o csv
check_eq "multi-column --sort accepts explicit ascending prefixes" 'region,product,amount,qty
East,Widget,10,2
East,Gadget,30,4
West,Widget,20,3
West,Gadget,40,5' "$out"

run "$sales" -B 'region,amount' -D -o csv
check_eq "-D applies to keys without an explicit prefix" 'region,product,amount,qty
West,Gadget,40,5
West,Widget,20,3
East,Gadget,30,4
East,Widget,10,2' "$out"

run "$people" --sort=-city,age -o csv
check_eq "a leading descending key can be attached with '='" 'id,name,age,city,score
3,Carol,35,Paris,72.25
4,dave,28,new york,100
1,Alice,30,New York,95.5
2,Bob,25,London,88
5,Eve,40,Berlin,' "$out"

run "$sales" -B 'nope,region' -o csv
check_status "multi-column --sort with one bad key still succeeds" 0 "$status"
check_contains "multi-column --sort warns about the bad key" "$err" \
    "Could not resolve sort column 'nope'"
check_eq "multi-column --sort still applies the valid keys" 'region,product,amount,qty
East,Widget,10,2
East,Gadget,30,4
West,Widget,20,3
West,Gadget,40,5' "$out"

# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

run "$sales" -A 'count,count(*),sum(amount),avg(amount),min(amount),max(amount)' -o csv
check_status "--aggregate exits successfully" 0 "$status"
check_eq "--aggregate supports every function" 'count,count(*),sum(amount),avg(amount),min(amount),max(amount)
4,4,100,25,10,40' "$out"

run "$sales" --aggregate 'sum(amount)' --output csv
check_eq "--aggregate accepts column names" 'sum(amount)
100' "$out"

run "$sales" -A 'sum(2)' -o csv
check_eq "--aggregate accepts numeric column indices" 'sum(2)
100' "$out"

run "$sales" -A 'count,sum(amount)' -w 'region = East' -o csv
check_eq "--aggregate respects WHERE filters" 'count,sum(amount)
2,40' "$out"

run "$sales" -n -A count -o csv
check_eq "aggregation takes precedence over --count" 'count
4' "$out"

run "$sales" -A count -S region -o csv
check_eq "aggregation ignores --select" 'count
4' "$out"

run "$sales" -A count -o json
check_eq "--aggregate renders in JSON" '[
  {"count": "4"}
]' "$out"

# ---------------------------------------------------------------------------
# Grouping
# ---------------------------------------------------------------------------

run "$sales" -g region -A 'count,sum(amount),avg(qty),min(qty),max(qty)' -o csv
check_status "--group-by exits successfully" 0 "$status"
check_eq "--group-by aggregates each group" 'region,count,sum(amount),avg(qty),min(qty),max(qty)
East,2,40,3,2,4
West,2,60,4,3,5' "$out"

run "$sales" --group-by region -o csv
check_eq "--group-by without an aggregate defaults to count" 'region,count
East,2
West,2' "$out"

run "$sales" -g 'region,product' -A 'sum(amount)' -o csv
check_eq "--group-by supports multiple columns" 'region,product,sum(amount)
East,Widget,10
West,Widget,20
East,Gadget,30
West,Gadget,40' "$out"

run "$sales" -g region -A 'count,sum(amount)' -w 'amount >= 20' -o csv
check_eq "--group-by respects WHERE filters" 'region,count,sum(amount)
West,2,60
East,1,30' "$out"

run "$duplicates" -g dept -U id -A count -o csv
check_eq "deduplication runs before grouping" 'dept,count
eng,2
sales,1' "$out"

run "$sales" -g region -A count -l 1 -o csv
check_eq "--limit applies to aggregate result rows" 'region,count
East,2' "$out"

run "$mixed" -g group -A 'count,sum(value),avg(value),min(value),max(value)' -o csv
check_eq "missing and non-numeric values are ignored by aggregates" 'group,count,sum(value),avg(value),min(value),max(value)
a,3,1,1,1,1
b,1,4,4,4,4
c,1,-,-,-,-' "$out"

run "$mixed" -A 'count,sum(value),avg(value),min(value),max(value)' -o csv
check_eq "aggregates over a whole file ignore missing values" 'count,sum(value),avg(value),min(value),max(value)
5,5,2.5,1,4' "$out"

run "$sales" -g region -A 'count,sum(amount)' -o json
check_eq "grouped aggregates render in JSON" '[
  {"region": "East", "count": "2", "sum(amount)": "40"},
  {"region": "West", "count": "2", "sum(amount)": "60"}
]' "$out"

run "$sales" -g region -A 'count,sum(amount)' -o markdown
check_eq "grouped aggregates render in Markdown" '| region | count | sum(amount) |
| --- | --- | --- |
| East | 2 | 40 |
| West | 2 | 60 |' "$out"

# ---------------------------------------------------------------------------
# Aggregation errors
# ---------------------------------------------------------------------------

run "$sales" -A 'median(amount)' -o csv
check_status "unknown aggregate functions fail" 1 "$status"
check_contains "unknown aggregate functions are reported" "$err" \
    "Invalid aggregate expression 'median(amount)'"

run "$sales" -A 'sum(nope)' -o csv
check_status "unknown aggregate columns fail" 1 "$status"
check_contains "unknown aggregate columns are reported" "$err" "Unknown aggregate column 'nope'"

run "$sales" -g nope -o csv
check_status "unknown group-by columns fail" 1 "$status"
check_contains "unknown group-by columns are reported" "$err" "Unknown group-by column 'nope'"

run "$sales" --aggregate= -o csv
check_status "an empty aggregate expression fails" 1 "$status"
check_contains "an empty aggregate expression is reported" "$err" \
    '--aggregate needs at least one expression'

run "$sales" --group-by= -o csv
check_status "an empty group-by column list fails" 1 "$status"
check_contains "an empty group-by column list is reported" "$err" \
    '--group-by needs at least one column'

# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

run
check_status "missing filename fails" 1 "$status"
check_contains "missing filename prints usage guidance" "$err" \
    'Required positional argument <filename> is missing'

run "$tmpdir/does-not-exist.csv"
check_status "missing input file fails" 1 "$status"
check_contains "missing input file reports the path" "$err" 'does-not-exist.csv'

: >"$tmpdir/empty.csv"
run "$tmpdir/empty.csv"
check_status "empty input file fails" 1 "$status"
check_contains "empty input file reports a parse failure" "$err" 'Failed to parse CSV file'

run "$ragged"
check_status "ragged CSV input fails" 1 "$status"
check_contains "ragged CSV input reports the invalid line" "$err" 'invalid number of fields'

run "$people" -l -1
check_status "negative --limit fails" 1 "$status"
check_contains "negative --limit reports its constraint" "$err" '--limit must be non-negative'

run "$people" -O nope
check_status "invalid --offset fails" 1 "$status"
check_contains "invalid --offset reports the bad value" "$err" "Invalid value for --offset: 'nope'"

run "$people" -l abc
check_status "non-numeric --limit fails" 1 "$status"
check_contains "non-numeric --limit reports the bad value" "$err" "Invalid value for --limit: 'abc'"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

printf '\n%d passed, %d failed, %d total\n' "$passed" "$failed" "$total"

if [ "$failed" -ne 0 ]; then
    exit 1
fi

exit 0
