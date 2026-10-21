//! genai (Rust) in the SDK comparison: `genai::Client::exec_chat` and `exec_chat_stream` on a tokio multi-thread runtime.
//!
//! Concurrency model: one `genai::Client` shared by `concurrency` tokio tasks, each running requests back to back.
//! TLS trust: the peer's CA is added to the `reqwest::Client` that is handed to `Client::builder().with_reqwest(..)`, which is
//! how genai's manifest says to use custom root certificates.
use std::io::{BufRead, Write};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use futures::StreamExt;
use genai::adapter::AdapterKind;
use genai::chat::{ChatMessage, ChatOptions, ChatRequest, ChatStreamEvent};
use genai::resolver::{AuthData, Endpoint};
use genai::{Client, ModelIden, ServiceTarget};

#[derive(Clone, Default)]
struct Args {
    api: String,
    mode: String,
    base_prefix: String,
    ca: String,
    model: String,
    system: String,
    user: String,
    expect: String,
    api_key: String,
    concurrency: usize,
    warmup: usize,
    requests: usize,
    max_tokens: u32,
    chunks: usize,
    cold: bool,
}

fn parse_args() -> Args {
    let mut a = Args { concurrency: 1, max_tokens: 256, chunks: 1, requests: 1, ..Default::default() };
    let mut it = std::env::args().skip(1);
    while let Some(key) = it.next() {
        if key == "--cold" {
            a.cold = true;
            continue;
        }
        let value = it.next().unwrap_or_else(|| panic!("missing value for {key}"));
        match key.as_str() {
            "--api" => a.api = value,
            "--mode" => a.mode = value,
            "--base-prefix" => a.base_prefix = value,
            "--ca" => a.ca = value,
            "--model" => a.model = value,
            "--system" => a.system = value,
            "--user" => a.user = value,
            "--expect" => a.expect = value,
            "--api-key" => a.api_key = value,
            "--concurrency" => a.concurrency = value.parse().unwrap(),
            "--warmup" => a.warmup = value.parse().unwrap(),
            "--requests" => a.requests = value.parse().unwrap(),
            "--max-tokens" => a.max_tokens = value.parse().unwrap(),
            "--chunks" => a.chunks = value.parse().unwrap(),
            other => panic!("unknown argument {other}"),
        }
    }
    a
}

fn status_kb(field: &str) -> i64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with(field)).and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse().ok())))
        .unwrap_or(-1)
}

fn usage_us() -> (i64, i64) {
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    (ru.ru_utime.tv_sec * 1_000_000 + ru.ru_utime.tv_usec, ru.ru_stime.tv_sec * 1_000_000 + ru.ru_stime.tv_usec)
}

fn json_string(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// What one request leaves behind: its latency in ns (0 = failed), the assembled text and the number of text deltas.
struct Sample {
    index: usize,
    ns: i64,
    text: String,
    deltas: usize,
}

struct Phase {
    samples: Vec<Sample>,
    failed: usize,
    first_error: Option<String>,
}

fn build_client(a: &Args) -> Client {
    let ca = std::fs::read(&a.ca).expect("read CA file");
    let http = reqwest::Client::builder()
        .add_root_certificate(reqwest::Certificate::from_pem(&ca).expect("CA certificate"))
        .http1_only()
        .pool_max_idle_per_host(a.concurrency)
        // the remaining settings are genai's own defaults (see its WebConfig)
        .tcp_nodelay(true)
        .gzip(true)
        .build()
        .expect("reqwest client");
    Client::builder().with_reqwest(http).build().expect("genai client")
}

fn target(a: &Args) -> ServiceTarget {
    let (adapter, base) = if a.api == "openai" {
        (AdapterKind::OpenAI, format!("{}/v1/", a.base_prefix))
    } else {
        (AdapterKind::Anthropic, format!("{}/v1/", a.base_prefix))
    };
    ServiceTarget { endpoint: Endpoint::from_owned(base), auth: AuthData::from_single(a.api_key.clone()), model: ModelIden::new(adapter, a.model.clone()) }
}

async fn one(client: &Client, a: &Args) -> Result<(i64, String, usize), String> {
    let t0 = Instant::now();
    let request = ChatRequest::new(vec![ChatMessage::user(a.user.clone())]).with_system(a.system.clone());
    let options = ChatOptions::default().with_max_tokens(a.max_tokens);
    if a.mode == "buffered" {
        let response = client.exec_chat(target(a), request, Some(&options)).await.map_err(|e| e.to_string())?;
        let ns = t0.elapsed().as_nanos() as i64;
        let text = response.into_first_text().unwrap_or_default();
        Ok((ns, text, 0))
    } else {
        let response = client.exec_chat_stream(target(a), request, Some(&options)).await.map_err(|e| e.to_string())?;
        let mut stream = response.stream;
        let (mut text, mut deltas) = (String::new(), 0usize);
        while let Some(event) = stream.next().await {
            match event.map_err(|e| e.to_string())? {
                ChatStreamEvent::Chunk(chunk) => {
                    if !chunk.content.is_empty() {
                        text.push_str(&chunk.content);
                        deltas += 1;
                    }
                }
                _ => {}
            }
        }
        Ok((t0.elapsed().as_nanos() as i64, text, deltas))
    }
}

async fn run_phase(client: Arc<Client>, a: Arc<Args>, count: usize, record: bool) -> Phase {
    let next = Arc::new(AtomicUsize::new(0));
    let mut tasks = Vec::new();
    for _ in 0..a.concurrency.min(count) {
        let (client, a, next) = (client.clone(), a.clone(), next.clone());
        tasks.push(tokio::spawn(async move {
            let (mut samples, mut failed, mut first_error) = (Vec::new(), 0usize, None);
            loop {
                let index = next.fetch_add(1, Ordering::Relaxed);
                if index >= count {
                    break;
                }
                match one(&client, &a).await {
                    Ok((ns, text, deltas)) => {
                        if record {
                            samples.push(Sample { index, ns, text, deltas });
                        }
                    }
                    Err(e) => {
                        failed += 1;
                        first_error.get_or_insert(e.chars().take(300).collect::<String>());
                    }
                }
            }
            (samples, failed, first_error)
        }));
    }
    let mut phase = Phase { samples: Vec::new(), failed: 0, first_error: None };
    for task in tasks {
        let (samples, failed, first_error) = task.await.expect("task");
        phase.samples.extend(samples);
        phase.failed += failed;
        if phase.first_error.is_none() {
            phase.first_error = first_error;
        }
    }
    phase
}

fn verify(a: &Args, text: &str, deltas: usize) -> bool {
    text == a.expect && (a.mode != "stream" || deltas == a.chunks)
}

fn read_line() -> String {
    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).expect("stdin");
    line
}

fn emit(line: &str) {
    let mut out = std::io::stdout().lock();
    out.write_all(line.as_bytes()).unwrap();
    out.write_all(b"\n").unwrap();
    out.flush().unwrap();
}

fn main() {
    let mut a = parse_args();
    if a.cold {
        a.concurrency = 1;
    }
    let runtime = tokio::runtime::Builder::new_multi_thread().enable_all().build().expect("tokio runtime");
    let client = Arc::new(build_client(&a));
    let a = Arc::new(a);

    if a.cold {
        let phase = runtime.block_on(run_phase(client, a.clone(), 1, true));
        let ok = phase.failed == 0 && phase.samples.first().is_some_and(|s| verify(&a, &s.text, s.deltas));
        emit(&format!("{{\"event\":\"first\",\"ok\":{ok}}}"));
        std::process::exit(if ok { 0 } else { 1 });
    }

    let (mut warm_failed, mut warm_error) = (0usize, None::<String>);
    let (mut rss_ready, mut hwm_ready);
    loop {
        if a.warmup > 0 {
            let phase = runtime.block_on(run_phase(client.clone(), a.clone(), a.warmup, false));
            warm_failed += phase.failed;
            warm_error = warm_error.or(phase.first_error);
        }
        rss_ready = status_kb("VmRSS:");
        hwm_ready = status_kb("VmHWM:");
        emit("{\"event\":\"ready\"}");
        let reply = read_line();
        if reply.starts_with("GO") {
            break;
        }
        if !reply.starts_with("MORE") {
            std::process::exit(3);
        }
    }

    let (u0, s0) = usage_us();
    let t0 = Instant::now();
    let phase = runtime.block_on(run_phase(client.clone(), a.clone(), a.requests, true));
    let wall = t0.elapsed().as_nanos() as i64;
    let (u1, s1) = usage_us();

    // Outside the measured window.
    let mut latency = vec![0i64; a.requests];
    let mut bad = 0usize;
    for s in &phase.samples {
        latency[s.index] = s.ns;
        if !verify(&a, &s.text, s.deltas) {
            bad += 1;
        }
    }
    let failed = phase.failed + warm_failed;
    let error = phase.first_error.clone().or(warm_error);
    let config = format!(
        "{{\"concurrency_model\":\"tokio multi-thread runtime ({} worker threads); one genai::Client shared by {} tasks\",\
\"http\":\"1.1 (reqwest http1_only)\",\"pool\":\"reqwest pool_max_idle_per_host={}\",\
\"ca\":\"reqwest add_root_certificate on the client passed to ClientBuilder::with_reqwest\",\
\"retries\":\"none in the crate\",\"telemetry\":\"none (the otel feature is off)\"}}",
        std::thread::available_parallelism().map(|n| n.get()).unwrap_or(0), a.concurrency, a.concurrency
    );
    let version = format!("0.7.1-WIP ({})", "06eca2124811");
    let mut out = format!(
        "{{\"event\":\"result\",\"schema\":1,\"client\":\"rust-genai\",\"version\":{},\"api\":{},\"mode\":{},\"concurrency\":{},\"warmup\":{},\"requests\":{},\"ok\":{},\"failed\":{},\"first_error\":{},\"verify_failed\":{},\"wall_ns\":{},\"cpu_user_us\":{},\"cpu_sys_us\":{},\"rss_ready_kb\":{},\"hwm_ready_kb\":{},\"rss_end_kb\":{},\"hwm_end_kb\":{},\"config\":{},\"latency_ns\":[",
        json_string(&version), json_string(&a.api), json_string(&a.mode), a.concurrency, a.warmup, a.requests, a.requests - phase.failed, failed,
        if failed > 0 { json_string(&error.unwrap_or_default()) } else { "null".to_string() }, bad, wall, u1 - u0, s1 - s0,
        rss_ready, hwm_ready, status_kb("VmRSS:"), status_kb("VmHWM:"), config
    );
    for (i, v) in latency.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&v.to_string());
    }
    out.push_str("]}");
    emit(&out);
    std::process::exit(0);
}
