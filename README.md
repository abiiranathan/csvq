# csvq 🔍

> A blazing fast, C-based command-line tool to pretty-print, filter, query, and convert CSV/TSV data.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C11-orange.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen.svg)

**csvq** is not just a pretty-printer. It's a lightweight CLI swiss-army knife for tabular data. Whether you need to inspect a messy CSV in your terminal, convert a dataset to JSON for an API, or generate Markdown tables for documentation, `csvq` does it instantly with a low memory footprint.

## 🚀 Features

*   **Pretty Printing**: Auto-calculated column widths and ANSI colors for readability.
*   **SQL-like Querying**: Filter rows using `where` clauses (e.g., `age > 25 AND role contains admin`).
*   **Multi-Format Export**: Convert CSV to:
    *   JSON (Array of objects)
    *   Markdown (GitHub flavored tables)
    *   TSV / CSV (Cleaned up)
*   **Sorting**: Sort by any column (Numeric or String) in Ascending or Descending order.
*   **Column Management**: Select, reorder, or hide specific columns.
*   **Fast & Efficient**: Written in C, optimized for speed and low memory usage.
*   **Robust Parsing**: Handles quoted fields, custom delimiters (including Tabs), and messy data.

## 📦 Installation

### Prerequisites
*   C Compiler (GCC/Clang): MSVC has not been tested but should work just fine.
*   [SolidC](https://github.com/abiiranathan/solidc) library (Required dependency)

### Building from Source

```bash
git clone https://github.com/abiiranathan/csvq.git
cd csvq
make
```

## 📖 Usage

### Basic Table View
Simply pass the filename to view a formatted ASCII table.
```bash
csvq data.csv
```

### Filtering Data (SQL-like)
Filter rows using logical operators (`>`, `<`, `=`, `!=`, `contains`).
```bash
# Find all products cheaper than $50 that mention "USB"
csvq inventory.csv --where "price < 50 AND item contains USB"
```

### Sorting
Sort data by a specific column index or name.
```bash
# Sort by 'Salary' in descending order
csvq employees.csv --sort Salary --desc
```

### Converting Formats
Export data for use in other tools.

**To JSON:**
```bash
csvq users.csv --output json > users.json
```

**To Markdown (Great for documentation):**
```bash
csvq metrics.csv --output markdown
```

### Column Selection
Pick only the columns you need, in the order you want.
```bash
# Select only Name (col 0) and Email (col 2)
csvq contacts.csv --select "Name,Email"
# OR using indices
csvq contacts.csv --select "0,2"
```

### Handling Tab-Separated Values (TSV)
```bash
csvq data.tsv --delimiter "\t"
```

## 🔧 Command Line Arguments

| Flag          | Short | Description                                              |
| ------------- | ----- | -------------------------------------------------------- |
| `--output`    | `-o`  | Output format: `table`, `json`, `markdown`, `csv`, `tsv` |
| `--where`     | `-w`  | Filter condition: `col_name > value`                     |
| `--sort`      | `-B`  | Column to sort by                                        |
| `--desc`      | `-D`  | Sort descending                                          |
| `--select`    | `-S`  | Columns to show/reorder (e.g., "id,name")                |
| `--hide`      | `-H`  | Columns to hide (e.g., "password")                       |
| `--filter`    | `-f`  | Simple regex-like row search                             |
| `--delimiter` | `-d`  | Custom delimiter (Default: `,`)                          |
| `--color`     | `-C`  | Enable colored columns                                   |

## 🤝 Contributing

Contributions are what make the open-source community such an amazing place to learn, inspire, and create. Any contributions you make are **greatly appreciated**.

1.  **Fork the Project**
2.  Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3.  Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4.  Push to the Branch (`git push origin feature/AmazingFeature`)
5.  Open a **Pull Request**

### Ideas for Contribution
*   Add support for HTML table output.
*   Add aggregate functions (Sum/Average of a column).

## 📜 License

Distributed under the MIT License. See `LICENSE` for more information.

## ✍️ Author

**Dr. Abiira Nathan**  
*07 December 2025*
