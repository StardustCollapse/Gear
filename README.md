# Gear

A systems-level programming language that compiles directly to x86-64 MASM assembly on Windows.  
Designed for bare-metal and OS-level programming with an emphasis on explicit memory control.

> [!NOTE]
> The majority of this compiler's implementation was generated with the assistance of a Large Language Model (LLM), guided and directed by the project author's design decisions and architectural constraints.

> [!NOTE]
> 本編譯器的大部分實作是在 LLM（大型語言模型）的輔助下產生的，由專案作者主導設計決策與架構約束。

---

## Build & Run

**Requirements**: Visual Studio 2022 (or MSVC Build Tools) with C++ Desktop workload installed.

```
# Build the compiler (from project root)
MSBuild Gear.sln /p:Configuration=Release /p:Platform=x64

# Compile and instantly run a Gear program
.\Bin\x64\Release\Compiler.exe Data\test.txt

# Compile only (no execution)
.\Bin\x64\Release\Compiler.exe Data\test.txt --no-run
```

The compiler automatically locates `vcvars64.bat` via `vswhere.exe` or standard paths, so it works outside the VS Developer Command Prompt.

---

## Project Structure

```
Gear/
├── Gear.sln                    # Visual Studio solution
├── Compiler/
│   ├── Code/
│   │   ├── Compiler.cpp        # Entry point, instant assemble-link-run pipeline
│   │   ├── Lexer.cpp           # Tokenizer
│   │   ├── Parser.cpp          # Recursive-descent parser (no AST, emits asm directly)
│   │   └── Generator.cpp       # MASM text/data section builder
│   └── Header/
│       ├── Compiler.h
│       ├── Lexer.h             # Token types and operators
│       ├── Parser.h            # Variable, function, and nested-function structs
│       └── Generator.h         # Emit API and global variable registry
├── Data/
│   └── test.txt                # Gear source code
├── Build/                      # Intermediate files (.asm, .obj) — auto-generated
└── Bin/                        # Executables (.exe) — auto-generated
```

---

## Compilation Pipeline

```
Source (.txt)
    │
    ▼
 ┌────────┐     ┌──────────────────────┐     ┌─────────┐
 │ Lexer  │ ──▶ │ Parser + Generator   │ ──▶ │  .asm   │
 │        │     │ (direct token walk,  │     │  file   │
 │ tokens │     │  no AST, emits MASM) │     │         │
 └────────┘     └──────────────────────┘     └────┬────┘
                                                  │
                                     ml64 + link  │
                                                  ▼
                                             ┌─────────┐
                                             │  .exe   │
                                             │ (runs)  │
                                             └─────────┘
```

**Key architectural constraint**: The parser is a recursive-descent token walker that emits x86-64 MASM assembly on the fly. **No AST is constructed.**

---

## Language Syntax Guide

### Variables

```gear
x = 42                  // 64-bit (8 bytes) by default
status:4 = 0            // explicit 4-byte variable
flag:1 = 0xFF           // explicit 1-byte variable
```

### Byte Slicing

Read or write a specific number of low bytes from a variable:

```gear
val = 0x12345678
lo_byte = val:1         // read low 1 byte  → 0x78
lo_word = val:2         // read low 2 bytes → 0x5678

val:2 = 0x1234          // overwrite ONLY the low 2 bytes → val becomes 0x12341234
```

### Global Variables

Defined at top level. Accessed and modified directly by name — no special syntax needed:

```gear
VGA_BUF = 0xB8000       // global variable (QWORD in .data)
HW_REG:4 = 0            // global variable (DWORD in .data)

// Inside functions:
VGA_BUF = 0xA0000       // read/write directly
print(VGA_BUF)
```

### Global Buffers

Allocate sized byte arrays in the `.data` segment:

```gear
SIM_VGA:4000 = 0        // allocates 4000 bytes (DB 4000 DUP(0) in MASM)
SIM_PKT:64 = 0          // allocates 64 bytes

ptr = [SIM_VGA]          // get the address of the buffer
```

### Functions

```gear
add(a, b) {
    return a + b
}

main() {                 // mandatory entry point
    result = add(x, y)
    return 0
}
```

Every Gear program **must** define `main()`. Omitting it produces a compile error.

---

### Parameter Duality: `a` vs `<a>`

This is Gear's core calling convention innovation.

**The caller always writes `func(x)` — never `func([x])`.**  
Under the hood, the compiler passes the *address* of `x`. The callee automatically gets **two views** of the same parameter:

| Syntax | What it accesses | Backed by |
|--------|-----------------|-----------|
| `a` | Local copy (safe to modify without side effects) | `[rbp - copy_offset]` |
| `<a>` | Caller's original data (modifications are visible to the caller) | Dereference of `[rbp - orig_offset]` |
| `[<a>]` | Memory address of caller's original data | Raw pointer value at `[rbp - orig_offset]` |

```gear
modify(val) {
    print(val)          // local copy:       0x55555555
    print(<val>)        // caller's original: 0x55555555

    val = 50            // only changes the local copy
    <val> = 0x99991234  // changes caller's original!

    print(val)          // 50
    print(<val>)        // 0x99991234
    // Same name, two different values simultaneously
}

main() {
    x = 0x55555555
    modify(x)
    print(x)            // 0x99991234 — caller's data was modified via <val>
    return 0
}
```

---

### Memory Operations with `[]`

The bracket syntax `[]` provides direct memory access, similar to assembly:

#### Take address

```gear
addr = [my_var]         // lea rax, [rbp + offset]  — address of local
addr = [GLOBAL]         // lea rax, [GLOBAL]        — address of global
addr = [<a>]            // mov rax, [rbp + orig]    — address of caller's original
```

#### Strided indexed access: `[base:stride]@index`

Read or write memory at `base + index × stride`:

```gear
// Read 2-byte value at VGA_BUF + i*2
cell = [VGA_BUF:2]@i

// Write 2-byte value at VGA_BUF + 0*2
[VGA_BUF:2]@0 = 0x0A47

// Read 4-byte value at payload_ptr + i*4
[payload_ptr:4]@i
```

#### Pointer dereference

```gear
[ptr] = value           // write 8 bytes to address in ptr
x = [ptr]               // read 8 bytes from address in ptr
[ptr:4] = value         // write 4 bytes
```

#### Struct-like field access

```gear
hdr = pkt_hdr[buf_addr]        // type-tagged pointer
field_val = hdr@0.ver_ihl      // read byte at hdr + offset 0
[hdr]->len                     // dereference hdr, access field 'len'
```

---

### Control Flow

```gear
// Conditional
if(x == 0) {
    // ...
} else {
    // ...
}

// Infinite loop with break
loop {
    if(done) { break }
    i = i + 1
}

// Return
return value
return -1
```

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Comparison | `==` `!=` `<` `>` `<=` `>=` |
| Logical | `&&` `\|\|` `!` |
| Compound Assignment | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |

---

### Pipeline Chains: `.then`

Chain a sequence of transformations, passing each result to the next stage:

```gear
[hdr]->len
    .then(len)   { if(len < 20) { return -2 } }
    .then(len)   { [buf_addr + 8:4]@0 }
    .then(value) { sum = sum + value }
```

Each `.then(param) { body }` receives the result of the previous expression (from RAX) into `param`, executes the body, and passes its result forward.

---

### Nested Routines: `.nested`

Define scoped helper routines within a function. They share the parent's local variables and stack frame:

```gear
handle_irq(irq_num, frame_addr) {
    status:4 = 0

    loop {
        nested read_hw_status;          // call nested routine

        if(status == 0) { break }

        if(status & 0x01) {
            nested handle_rx_packet;
        }

        nested ack_interrupt;
    }

    .nested {                           // define nested routines
        read_hw_status {
            status = [APIC_BASE:4]@0x10
        },
        handle_rx_packet {
            pkt = rx_desc[frame_addr]
            nested copy_to_ring_buffer;
        },
        ack_interrupt {
            [APIC_BASE:4]@0xB0 = 0
        }
    }
    .nested {                           // multiple .nested blocks allowed
        copy_to_ring_buffer {
            [APIC_BASE:4]@0x10 = 0
        }
    }
}
```

Nested routines compile to labeled code blocks with `call`/`ret` within the parent procedure — no extra stack frame overhead.

---

### Built-in: `print`

```gear
print(expr)
// Output format: [Gear Output] 0xHEX (DECIMAL)
```

Calls C runtime `printf` via MSVCRT linkage.

---

## Example: Full Test Program

See [`Data/test.txt`](Data/test.txt) for the complete test suite that exercises:

- Arithmetic and byte slicing
- Parameter duality (`a` / `<a>` / `[<a>]`)
- Global variable access
- VGA text-mode screen clearing (simulated with user-space buffer)
- Hardware interrupt handling with `.nested` routines (simulated with user-space buffer)
- Network packet processing with `.then` pipelines (simulated with user-space buffer)

```
.\Bin\x64\Release\Compiler.exe Data\test.txt

[Gear Lexer] 595 tokens parsed.
[Gear Generator] MASM output written to: .\Build\test.asm
 Assembling: .\Build\test.asm

================ [Gear Instant Run] ================
[Gear Output] 0x17F (383)
[Gear Output] 0x78 (120)
[Gear Output] 0x5678 (22136)
[Gear Output] 0x12341234 (305402420)
[Gear Output] 0x55555555 (1431655765)
[Gear Output] 0x55555555 (1431655765)
[Gear Output] 0x32 (50)
[Gear Output] 0x99991234 (2576945716)
[Gear Output] 0x99991234 (2576945716)
[Gear Output] 0xABCD (43981)
[Gear Output] 0xA47 (2631)
[Gear Output] 0x0 (0)
[Gear Output] 0x0 (0)
[Gear Output] 0x1E (30)
[Gear Output] 0x1E (30)
====================================================
```

---

## Target ABI

Windows x64 calling convention:

- **Parameters**: RCX, RDX, R8, R9 (first 4), then stack
- **Return value**: RAX
- **Shadow space**: 32 bytes (caller-allocated)
- **Stack alignment**: 16-byte aligned at `CALL`
- **Volatile**: RAX, RCX, RDX, R8, R9, R10, R11
- **Non-volatile (callee-saved)**: RBX, RBP, RDI, RSI, R12–R15

---

## License

This project is provided as-is for educational and experimental purposes.
