use crate::{alphabet::Alphabet, constant::CHARS_TO_LANGUAGES_MAPPING, language::Language};
use fst::Streamer;
use std::{fs, io::{BufWriter, Write}, collections::BTreeMap};
use strum::IntoEnumIterator;

pub fn languages() -> Vec<Language> { Language::iter().collect() }
fn mask(langs: impl Iterator<Item=Language>) -> (u64,u64) {
    let mut m = (0,0);
    for l in langs { let i = l as usize; if i < 64 { m.0 |= 1 << i; } else { m.1 |= 1 << (i-64); } }
    m
}
pub fn run(rust_root: &str, output: &str, only_metadata: bool) {
    let mut header = BufWriter::new(fs::File::create(format!("{output}/../generated/metadata.hpp")).unwrap());
    writeln!(header, "// Generated from lingua-rs and its Unicode tables. Do not edit.\n#pragma once\n#include <cstdint>\nnamespace lingua {{\nenum class Language : int {{").unwrap();
    let langs = languages();
    for l in &langs { writeln!(header, "{l},").unwrap(); }
    writeln!(header, "Unknown = -1 }};\ninline constexpr int language_count = {};\ninline constexpr const char* language_names[] = {{", langs.len()).unwrap();
    for l in &langs { writeln!(header,"\"{l}\",").unwrap(); }
    writeln!(header,"}};\nnamespace generated {{\nstruct Range {{ uint32_t start, end; uint8_t script, flags; }};\ninline constexpr Range ranges[] = {{").unwrap();
    let mut scripts = vec![255u8; 0x110000];
    let alphabets: Vec<_> = Alphabet::iter().collect();
    for (i,a) in alphabets.iter().enumerate() {
        let name = format!("{a:?}");
        let ranges = crate::script::BY_NAME.iter().find(|(n,_)| *n == name).unwrap().1;
        for &(start,end) in ranges { for cp in start as usize..=end as usize { scripts[cp]=i as u8; } }
    }
    let mut flags = vec![0u8; 0x110000];
    let all: String = (0..0x110000).filter_map(char::from_u32).collect();
    for (pattern, flag) in [(r"\p{L}",1), (r"\p{Cased}",2), (r"\p{Case_Ignorable}",4)] {
        for mat in regex::Regex::new(pattern).unwrap().find_iter(&all) { for c in mat.as_str().chars() { flags[c as usize] |= flag; } }
    }
    // Tokenization uses regex's Unicode version; the rule engine uses the
    // checkout's separately generated script.rs. They need not be identical.
    for (i, name) in ["Bengali","Devanagari","Gujarati","Gurmukhi","Han","Hangul","Hiragana","Katakana","Tamil","Telugu","Thai"].iter().enumerate() {
        for mat in regex::Regex::new(&format!(r"\p{{{name}}}")).unwrap().find_iter(&all) {
            for c in mat.as_str().chars() { flags[c as usize] |= ((i+1) as u8) << 4; }
        }
    }
    let mut start=0;
    while start < scripts.len() {
        let mut end=start;
        while end+1 < scripts.len() && scripts[end+1]==scripts[start] && flags[end+1]==flags[start] { end+=1; }
        if scripts[start]!=255 || flags[start]!=0 { writeln!(header,"{{{start},{end},{},{}}},",scripts[start],flags[start]).unwrap(); }
        start=end+1;
    }
    writeln!(header,"}};\nstruct Lower {{ uint32_t cp, a, b, c; }};\ninline constexpr Lower lower[] = {{").unwrap();
    for c in (0..0x110000).filter_map(char::from_u32) {
        let lower: Vec<_> = c.to_lowercase().map(|c|c as u32).collect();
        if lower != [c as u32] { writeln!(header,"{{{},{},{},{}}},", c as u32, lower[0],lower.get(1).unwrap_or(&0),lower.get(2).unwrap_or(&0)).unwrap(); }
    }
    writeln!(header,"}};\nstruct Mask {{ uint64_t lo, hi; }};\ninline constexpr Mask script_languages[] = {{").unwrap();
    for a in &alphabets { let m=mask(langs.iter().copied().filter(|l|l.alphabets().contains(a))); writeln!(header,"{{{}ULL,{}ULL}},",m.0,m.1).unwrap(); }
    writeln!(header,"}};\nstruct Rule {{ uint32_t cp; Mask unique, filter; }};\ninline constexpr Rule rules[] = {{").unwrap();
    let mut rules = BTreeMap::new();
    for l in &langs { if let Some(chars)=l.unique_characters() { for c in chars.chars() { rules.entry(c).or_insert((Vec::new(),Vec::new())).0.push(*l); } } }
    for (chars, ls) in CHARS_TO_LANGUAGES_MAPPING.iter() { for c in chars.chars() { rules.entry(c).or_insert((Vec::new(),Vec::new())).1.extend(ls); } }
    for (c,(unique,filter)) in rules { let u=mask(unique.into_iter()); let f=mask(filter.into_iter()); writeln!(header,"{{{},{{{}ULL,{}ULL}},{{{}ULL,{}ULL}}}},",c as u32,u.0,u.1,f.0,f.1).unwrap(); }
    writeln!(header,"}};\n}}\n}}").unwrap();
    if only_metadata { return; }
    let mut out = BufWriter::new(fs::File::create(format!("{output}/ngrams.raw")).unwrap());
    let mut count=0u64;
    for l in langs {
        let code=l.iso_code_639_1().to_string().to_lowercase();
        let map=fst::Map::new(fs::read(format!("{rust_root}/language-models/{code}/models/ngrams.fst")).unwrap()).unwrap();
        let mut stream=map.stream();
        while let Some((key,value))=stream.next() {
            let chars: Vec<_>=std::str::from_utf8(key).unwrap().chars().map(|c|c as u32+1).collect();
            assert!(!chars.is_empty() && chars.len()<=5);
            let mut lo=0u64; let mut hi=0u64;
            for (i,cp) in chars.into_iter().enumerate() { if i<3 {lo|=(cp as u64)<<(21*i);} else {hi|=(cp as u64)<<(21*(i-3));} }
            for bytes in [lo.to_le_bytes(),hi.to_le_bytes(),value.to_le_bytes()] {out.write_all(&bytes).unwrap();}
            out.write_all(&(l as u64).to_le_bytes()).unwrap(); count+=1;
        }
    }
    eprintln!("Exported {count} exact f64 probabilities");
}
