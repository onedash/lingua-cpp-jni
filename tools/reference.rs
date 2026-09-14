use lingua_reference::{export, LanguageDetectorBuilder};
use std::{fs, time::Instant, hint::black_box, io::Write};
fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args[1] == "export" || args[1] == "metadata" { export::run(&args[2], &args[3], args[1] == "metadata"); return; }
    let texts: Vec<String> = fs::read_to_string(&args[2]).unwrap().lines().map(str::to_owned).collect();
    let detector = LanguageDetectorBuilder::from_all_languages().with_preloaded_language_models().build();
    if args[1] == "verify" {
        let mut out = std::io::BufWriter::new(fs::File::create(&args[3]).unwrap());
        for text in &texts {
            let values = detector.compute_language_confidence_values(text);
            let found = detector.detect_language_of(text);
            write!(out, "{}", found.map(|l| l.to_string()).unwrap_or("None".into())).unwrap();
            for language in export::languages() {
                write!(out, "\t{:.17e}", values.iter().find(|v| v.0 == language).unwrap().1).unwrap();
            }
            writeln!(out).unwrap();
        }
        return;
    }
    let threads: usize = args[3].parse().unwrap();
    let rounds: usize = args[4].parse().unwrap();
    for text in &texts { black_box(detector.detect_language_of(text)); }
    let barrier = std::sync::Barrier::new(threads + 1);
    std::thread::scope(|scope| {
        let mut handles = Vec::new();
        for worker in 0..threads {
            let (texts, detector, barrier) = (&texts, &detector, &barrier);
            handles.push(scope.spawn(move || {
                barrier.wait();
                for _ in 0..rounds {
                    for i in (worker..texts.len()).step_by(threads) {
                        black_box(detector.detect_language_of(&texts[i]));
                    }
                }
            }));
        }
        let start = Instant::now();
        barrier.wait();
        for handle in handles { handle.join().unwrap(); }
        let seconds = start.elapsed().as_secs_f64();
        println!("threads={threads} requests={} seconds={seconds:.6} rps={:.3}", texts.len()*rounds, (texts.len()*rounds) as f64/seconds);
    });
}
