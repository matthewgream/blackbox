#!/usr/bin/env node
/*
 * csv2json — turn blackbox CSV records into JSON, using the project's record header for the keys.
 *
 * Records are stored keyless (`tag,clock,col,col,…`); this reads a C header that declares each
 * record struct, annotated so the CSV tag maps to the right struct + field names:
 *
 *     // @blackbox tag=LC
 *     typedef struct { uint8_t reason; uint16_t bootcount; } lc_event_t;
 *
 * The struct's field names become the JSON keys (numeric C types → numbers, else strings). Dynamic
 * (the header is parsed each run — fast, no codegen). Usable as a CLI or an importable module.
 *
 *   CLI:     csv2json --definitions=records.h < records.csv > records.json
 *   module:  const { parseDefinitions, csvLineToJson } = require('blackbox/js/csv2json');
 *
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */
'use strict';

function isNumericType(t) {
    return /^(u?int(8|16|32|64)?_t|u?int|short|long|float|double|size_t|bool)$/.test(t);
}

// Parse a record header → { TAG: [ {name, type}, … ] } in declaration (== CSV column) order.
function parseDefinitions(text) {
    const schema = {};
    const re = /@blackbox\s+tag=(\w+)[\s\S]*?struct[^{]*\{([^}]*)\}/g;
    let m;
    while ((m = re.exec(text)) !== null) {
        const tag = m[1];
        const fields = [];
        for (let decl of m[2].split(';')) {
            decl = decl.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '').trim();
            if (!decl) continue;
            const sp = decl.search(/\s/);
            if (sp < 0) continue;
            let type = decl.slice(0, sp).trim();
            let rest = decl.slice(sp).trim();
            if (type === 'const' || type === 'unsigned' || type === 'signed') { // e.g. "const char *kv"
                const sp2 = rest.search(/\s/);
                type = rest.slice(0, sp2 < 0 ? rest.length : sp2).trim();
                rest = sp2 < 0 ? '' : rest.slice(sp2).trim();
            }
            const isPtr = rest.includes('*') || decl.includes('char');
            for (let nm of rest.split(',')) {
                nm = nm.replace(/[*\[\]0-9]/g, '').trim();
                if (nm) fields.push({ name: nm, type, numeric: isNumericType(type) && !isPtr });
            }
        }
        schema[tag] = fields;
    }
    return schema;
}

// One CSV line → object. clock + tag always included; unknown tags keep raw columns.
function csvLineToJson(line, schema) {
    const cols = line.split(',');
    const tag = cols[0], clock = cols[1];
    const obj = { tag, clock };
    const fields = schema[tag];
    if (!fields) { obj._cols = cols.slice(2); return obj; }
    for (let i = 0; i < fields.length; i++) {
        const v = cols[2 + i];
        if (v === undefined) break;
        obj[fields[i].name] = fields[i].numeric ? Number(v) : v;
    }
    return obj;
}

function csvToJson(csvText, schema) {
    return csvText.split('\n').map(l => l.trim()).filter(Boolean).map(l => csvLineToJson(l, schema));
}

module.exports = { parseDefinitions, csvLineToJson, csvToJson, isNumericType };

if (require.main === module) {
    const fs = require('fs');
    const args = process.argv.slice(2);
    let defs = null;
    for (let i = 0; i < args.length; i++) {
        if (args[i] === '--definitions') defs = args[++i];
        else if (args[i].startsWith('--definitions=')) defs = args[i].slice('--definitions='.length);
    }
    if (!defs) { process.stderr.write('usage: csv2json --definitions=<records.h> < csv > json\n'); process.exit(2); }
    const schema = parseDefinitions(fs.readFileSync(defs, 'utf8'));
    const input = fs.readFileSync(0, 'utf8');
    process.stdout.write(JSON.stringify(csvToJson(input, schema), null, 2) + '\n');
}
