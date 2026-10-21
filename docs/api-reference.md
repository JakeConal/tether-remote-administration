# API reference

The public surface of every tether library. Signatures are quoted from the
headers; semantics are described where the signature alone is not enough.

All namespaces follow the directory layout: `tether::common`,
`tether::protocol`, `tether::transport`, `tether::syscalls`, `tether::cli`,
`tether::agent`.

---

## 1. `tether-common`

`lib/common/` — configuration, logging, base64.

### Configuration (`config.hpp`)

```cpp
struct GmailSectionConfig {
    bool enabled = false;
    std::string credentials_path;
    int poll_interval_seconds = 0;
    std::vector<std::string> admin_list;
    std::string default_agent;
};

struct ConsoleConfig {
    std::string listen_address;
    int port = 0;
    std::string token_store_path;
    std::optional<GmailSectionConfig> gmail;
    std::optional<std::string> audit_path;
    int agent_dial_wait_ms = 10000;
};

struct AgentConfig {
    std::string console_address;
    int console_port = 47474;
    std::string agent_name;
    std::string instance_id;   // defaults to agent_name when absent
    std::string token;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ConsoleConfig load_console_config(const std::string& path);
AgentConfig   load_agent_config(const std::string& path);
```

Both loaders throw `ConfigError` with a message that names the file and, for
field-level problems, the field. Full rules in
[configuration.md](configuration.md).

### Logging (`logging.hpp`)

```cpp
enum class LogLevel { Trace, Debug, Info, Warn, Error };

const char* log_level_name(LogLevel level);

struct LogField { std::string name; std::string value; };

struct LogRecord {
    LogLevel level;
    std::string timestamp;   // ISO-8601 UTC, e.g. 2026-10-20T12:34:56Z
    std::string message;
    std::vector<LogField> fields;
};

std::string format_log_record(const LogRecord& record);

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

class StreamLogSink : public LogSink {
public:
    explicit StreamLogSink(std::ostream& out);
    void write(const LogRecord& record) override;
};

std::string utc_timestamp_now();

class Logger {
public:
    static Logger& instance();
    void write(LogLevel level, const std::string& message,
               std::vector<LogField> fields = {});
    void trace/debug/info/warn/error(const std::string& message,
                                     std::vector<LogField> fields = {});
    void set_sink(std::unique_ptr<LogSink> sink);
    void set_min_level(LogLevel level);
    bool enabled(LogLevel level) const;
};
```

**Rendered format** — one line per record:

```
2026-10-20T12:34:56Z [info] command dispatched session=agent-1 seq=7
```

The default sink writes to **stderr**; the default minimum level is `Info`.
`Logger::instance()` is a process-wide singleton — tests replace the sink and
do not restore it.

### Base64 (`base64.hpp`)

```cpp
class Base64Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string base64_encode(const unsigned char* bytes, std::size_t count,
                           bool url_safe = false, bool pad = true);
std::string base64_encode_string(const std::string& text,
                                  bool url_safe = false, bool pad = true);
std::vector<unsigned char> base64_decode(const std::string& text);
std::string base64_decode_string(const std::string& text);
```

- `url_safe` selects the RFC 4648 §5 alphabet (`-` and `_`).
- `pad = false` drops the `=` padding.
- Decoding tolerates whitespace, accepts both alphabets mixed in one string,
  and stops at the first `=`.

---

## 2. `tether-protocol`

`lib/protocol/` — the wire. See [protocol.md](protocol.md) for the byte-level
specification.

### Bytes (`bytes.hpp`)

```cpp
struct ReadResult {
    std::size_t count = 0;
    bool end_of_stream = false;
};

class IByteSource {
public:
    virtual ~IByteSource() = default;
    virtual ReadResult read(unsigned char* dst, std::size_t max) = 0;
};

class IByteSink {
public:
    virtual ~IByteSink() = default;
    virtual void write(const unsigned char* bytes, std::size_t count) = 0;
};

class InMemoryPipe : public IByteSource, public IByteSink {
public:
    ReadResult read(unsigned char* dst, std::size_t max) override;
    void write(const unsigned char* bytes, std::size_t count) override;
    void close_writer();
};

struct InMemoryConnection {
    InMemoryPipe to_console;   // agent writes here, console reads
    InMemoryPipe to_agent;     // console writes here, agent reads
};
```

`read()` never blocks: `{0, false}` means "nothing available right now".
`InMemoryPipe` is **not** thread-safe by design.

### Envelope (`envelope.hpp`)

```cpp
inline constexpr unsigned char kEnvelopeVersion = 2;
inline constexpr std::size_t   kEnvelopeHeaderBytes = 10;
inline constexpr std::size_t   kMaxControlPayloadBytes = 1024 * 1024;
inline constexpr std::size_t   kMaxFileChunkBytes = 256 * 1024;

enum class MsgType : unsigned char {
    Hello = 1, Challenge = 2, AuthOk = 3, Cmd = 4, ResultText = 5,
    FileMeta = 6, FileChunk = 7, FileEnd = 8, Cancel = 9,
    Ping = 10, Pong = 11, Err = 12,
};

const char* msg_type_name(MsgType type);
bool is_valid_msg_type(unsigned char raw);
std::size_t max_payload_bytes_for(MsgType type);

class ProtocolError : public std::runtime_error {
public:
    enum class Code {
        UnknownVersion, OversizePayload, Truncated,
        MalformedPayload, BadArgument, CryptoFailure, UnknownType,
    };
    ProtocolError(Code code, const std::string& message);
    Code code() const noexcept;
};

struct Frame {
    MsgType type = MsgType::Hello;
    std::uint32_t seq = 0;
    std::vector<unsigned char> payload;
};

std::vector<unsigned char> encode_frame(const Frame& frame);

class FrameDecoder {
public:
    void feed(const unsigned char* bytes, std::size_t count,
              std::vector<Frame>& frames);
    std::size_t buffered() const noexcept;
    void finish();
};
```

`FrameDecoder::feed` tolerates **arbitrary slice boundaries** — feeding one
byte at a time is equivalent to feeding everything at once. A framing
violation poisons the decoder: the error is re-thrown on every subsequent
call and no further frame is ever emitted.

### Payloads (`payloads.hpp`)

```cpp
inline constexpr std::size_t kNonceHexChars = 32;
inline constexpr std::size_t kHmacHexChars = 64;

struct Hello    { unsigned protocol_version = kEnvelopeVersion;
                  std::string agent_name; std::string agent_id; };
struct Challenge{ std::string nonce; std::string hmac_response; };
struct AuthOk   {};
struct Cmd      { std::string command; std::vector<std::string> args; };
struct ResultText{ std::string text; };
struct FileMeta { std::string name; std::uint64_t size = 0; };
struct FileChunk{ std::vector<unsigned char> bytes; };
struct FileEnd  {};
struct Cancel   {};
struct Ping     {};
struct Pong     {};
struct Err      { std::string code; std::string message; };

using Message = std::variant<Hello, Challenge, AuthOk, Cmd, ResultText, FileMeta,
                             FileChunk, FileEnd, Cancel, Ping, Pong, Err>;

MsgType message_type_of(const Message& message);
std::vector<unsigned char> encode_payload(const Message& message);
std::vector<unsigned char> encode_message(const Message& message, std::uint32_t seq);
Message decode_payload(MsgType type, const std::vector<unsigned char>& payload);
```

`decode_payload` throws `ProtocolError(MalformedPayload)` with a message that
**names the offending field**. Unknown extra fields are rejected — there is
no lenient mode.

### Handshake (`handshake.hpp`)

```cpp
inline constexpr std::size_t kHmacBytes = 32;

std::array<unsigned char, kHmacBytes> hmac_sha256(const std::string& token,
                                                  const unsigned char* raw,
                                                  std::size_t len);
std::string hmac_sha256_hex(const std::string& token, const std::string& raw);
std::string generate_nonce_hex();
bool is_valid_nonce_hex(const std::string& nonce_hex);

enum class HandshakeFailure {
    NotBegun, UnknownNonce, NonceReplay, BadHmac, WrongToken,
};

class Challenger {
public:
    explicit Challenger(std::string token);
    Challenge begin();                                  // single-shot
    std::optional<HandshakeFailure> verify(const Challenge& answer, AuthOk& out_ok);
};

class Responder {
public:
    explicit Responder(std::string token);
    Challenge respond(const Challenge& challenge);
};
```

The HMAC covers the **raw decoded nonce bytes**, not their hex
representation. The token is used verbatim as the HMAC key — there is no key
derivation step.

### Session (`session.hpp`)

```cpp
using SessionMillis = std::int64_t;

struct SessionConfig {
    SessionMillis command_timeout    = 30'000;
    SessionMillis auth_timeout       = 10'000;
    SessionMillis keepalive_interval = 10'000;
    SessionMillis keepalive_window   = 30'000;
    void validate() const;   // throws SessionError
};

enum class SessionState { Connecting, Established, Stale, Down };
const char* session_state_name(SessionState state);

using ResultHook = std::function<void(std::uint32_t seq, const Message& piece,
                                       bool terminal)>;
using TokenResolver = std::function<std::string(const std::string& agent_name)>;

class SessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class CommandStatus { Pending, Completed, Failed, Cancelled };

struct CommandView {
    std::uint32_t seq = 0;
    CommandStatus status = CommandStatus::Pending;
    std::string command;
    std::vector<Message> pieces;
    std::string error_code;
};

struct CommandOptions { std::optional<SessionMillis> timeout; };

class ConsoleSession {
public:
    ConsoleSession(IByteSource& source, IByteSink& sink,
                   SessionConfig config, TokenResolver resolve_token);
    void pump(SessionMillis now);
    SessionState state() const noexcept;
    const std::string& identity() const noexcept;
    std::uint32_t start(const Cmd& command, SessionMillis now,
                        const ResultHook& hook = nullptr,
                        const CommandOptions& options = {});
    bool cancel(std::uint32_t seq);
    std::optional<CommandView> command(std::uint32_t seq) const;
    std::vector<std::uint32_t> outstanding_seqs() const;
    void mark_stale();
};

using CommandHandler = std::function<void(std::uint32_t seq, const Cmd& command,
                                          const std::function<bool()>& is_cancelled)>;

struct AgentIdentity { std::string name; std::string id; };

class AgentSession {
public:
    AgentSession(IByteSource& source, IByteSink& sink, AgentIdentity identity,
                 std::string token, SessionConfig config);
    void begin(SessionMillis now);       // throws SessionError on an empty token
    void pump(SessionMillis now);
    SessionState state() const noexcept;
    void set_command_handler(CommandHandler handler);
    void send_result(std::uint32_t seq, const Message& piece);
};

enum class RegistryState { Live, Stale, Down };
const char* registry_state_name(RegistryState state);

struct RegistryEntry { std::string identity; RegistryState state = RegistryState::Down; };

class AgentRegistry {
public:
    bool register_session(const std::string& identity,
                          const std::shared_ptr<ConsoleSession>& session);
    std::shared_ptr<ConsoleSession> lookup(const std::string& identity) const;
    std::size_t size() const noexcept;
    bool has_agent(const std::string& identity) const;
    RegistryState state_of(const std::string& identity) const;
    std::vector<RegistryEntry> list();
};
```

`register_session` returns `true` when it **superseded** an older
`Established` session for the same identity (that one is marked `Stale`).
The registry keeps no parallel state copy — the visible state is derived from
`session.state()` at query time.

---

## 3. `tether-transport`

`lib/transport/`.

```cpp
using TransportMillis = std::int64_t;

TransportMillis monotonic_now_ms();

class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual std::uint32_t issue(const std::string& agent,
                                const protocol::Cmd& command,
                                const protocol::ResultHook& hook,
                                TransportMillis now) = 0;
    virtual void pump(TransportMillis now) = 0;
};

struct ConsoleTransportSet {
    std::unique_ptr<ITransport> tcp;
    std::unique_ptr<ITransport> gmail;
    bool gmail_enabled = false;
};

ConsoleTransportSet build_console_transports(
    const common::ConsoleConfig& config,
    const protocol::SessionConfig& session_config,
    const protocol::TokenResolver& tokenize,
    const std::function<std::unique_ptr<GmailClient>()>& make_gmail_client);
```

### TCP (`tcp.hpp`)

```cpp
class TcpByteStream : public protocol::IByteSource, public protocol::IByteSink {
public:
    // read()/write()/shutdown() as per the byte-stream interfaces
};

class TcpConsoleTransport : public ITransport {
public:
    explicit TcpConsoleTransport(ConsoleTransportOptions options);
    void start();                       // resolve → open → bind → listen → accept
    std::uint16_t listening_port() const noexcept;
    // ITransport:
    std::uint32_t issue(...) override;
    void pump(TransportMillis now) override;
};

class TcpAgentLink {
public:
    static std::unique_ptr<TcpAgentLink> connect(
        const AgentEndpoint& endpoint, const protocol::AgentIdentity& identity,
        const std::string& token, protocol::SessionConfig config);
    void begin_handshake(TransportMillis now);
    void pump(TransportMillis now);
    void send_result(std::uint32_t seq, const protocol::Message& piece);
    bool established() const noexcept;
    bool down() const noexcept;
    protocol::SessionState state() const noexcept;
};

struct DialPolicy {
    std::int64_t initial_backoff_ms = 500;
    std::int64_t max_backoff_ms = 10'000;
};

std::int64_t next_backoff_ms(std::int64_t current, const DialPolicy& policy);

std::unique_ptr<TcpAgentLink> establish_with_retries(
    const AgentEndpoint& endpoint, const protocol::AgentIdentity& identity,
    const std::string& token, const protocol::SessionConfig& config,
    const DialPolicy& policy, std::size_t max_attempts = 0);
```

`max_attempts == 0` means **retry forever**. The backoff sequence is
500 → 1000 → 2000 → 4000 → 8000 → 10000 → 10000 … ms.

### Gmail (`gmail.hpp`)

```cpp
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual std::string post_form(const std::string& url,
                                  const std::vector<std::string>& form_fields) = 0;
    virtual std::string get(const std::string& url,
                            const std::vector<std::string>& headers) = 0;
    virtual std::string post_json(const std::string& url,
                                  const std::vector<std::string>& headers,
                                  const std::string& body) = 0;
};

class GmailClient {
public:
    virtual ~GmailClient() = default;
    virtual std::vector<std::string> list_inbox_ids(std::size_t max_results) = 0;
    virtual GmailMessage fetch(const std::string& id) = 0;
    virtual void send_raw(const std::string& raw_mime) = 0;
};

class GmailTransport final : public ITransport {
public:
    // issue() ALWAYS throws: mail is the issuance mechanism, not the
    // programmatic one. pump() performs at most one poll cycle.
};

std::shared_ptr<HttpClient>   make_curl_http_client();
std::shared_ptr<GmailClient>  make_google_gmail_client(
    std::shared_ptr<HttpClient> http, const GmailCredentials& credentials);
```

The acceptance rule: the sender must be on `admin_list` **and** the `To`
header must contain the adapter account (whole-address, case-insensitive).
Anything else is ignored — **no execution, no acknowledgement**.

---

## 4. `tether-syscalls`

`lib/syscalls/`.

### The matrix (`matrix.hpp`)

```cpp
enum class Platform { Windows, Linux };
enum class Family { Process, File, Power, Capture, Camera, App, Input };

struct CommandSpec {
    const char* verb;
    Family family;
    bool destructive;
    bool on_windows;
    bool on_linux;
};

inline constexpr const char* kConfirmMarker = "confirm";

const std::vector<CommandSpec>& command_set();
const CommandSpec* find_command(const std::string& verb);

struct Outcome {
    bool ok = false;
    std::string code = "internal_error";
    std::string message;
    std::vector<std::string> lines;
    struct Deliverable {
        std::string name;
        std::vector<unsigned char> bytes;
    };
    std::optional<Deliverable> file;
};

class CommandDispatcher {
public:
    explicit CommandDispatcher(Platform platform = current_platform(),
                               const std::vector<CommandSpec>& table = command_set());
    void set_process_control(std::shared_ptr<IProcessControl> impl);
    void set_file_system(std::shared_ptr<IFileSystem> impl);
    void set_power_control(std::shared_ptr<IPowerControl> impl);
    void set_screen_capture(std::shared_ptr<IScreenCapture> impl);
    void set_camera_capture(std::shared_ptr<ICameraCapture> impl);
    void set_input_control(std::shared_ptr<IInputControl> impl);
    void set_app_launcher(std::shared_ptr<IAppLauncher> impl);
    Outcome dispatch(const std::string& verb,
                     const std::vector<std::string>& args) const;
    Platform platform() const noexcept;
    const std::vector<CommandSpec>& table() const noexcept;
};
```

### Errors (`errors.hpp`)

```cpp
class SyscallError : public std::runtime_error {
public:
    SyscallError(std::string code, const std::string& message);
    const std::string& code() const noexcept;
};
```

The `code` is one of the refusal codes in
[command-reference.md §5](command-reference.md#5-refusal-codes).

### Jobs (`jobs.hpp`)

```cpp
struct JobDeliverable {
    bool ready = false;
    std::string name;
    std::vector<unsigned char> bytes;
};

class JobHandle {
public:
    void request_stop();
    bool stop_requested() const;
    void publish(JobDeliverable deliverable);
    std::optional<JobDeliverable> latest() const;
    void fail(const std::string& reason);
    std::optional<std::string> failure() const;
};

class BackgroundJobRegistry {
public:
    using Worker = std::function<void(JobHandle&)>;
    BackgroundJobRegistry();
    ~BackgroundJobRegistry();
    bool start(const std::string& slot, Worker worker);
    struct StopOutcome {
        bool was_running = false;
        std::optional<JobDeliverable> deliverable;
        std::optional<std::string> failure;
    };
    StopOutcome stop(const std::string& slot);
    bool running(const std::string& slot) const;
    static constexpr const char* kCameraSlot = "camera";
    static constexpr const char* kInputSlot = "input";
};

std::shared_ptr<BackgroundJobRegistry> shared_job_registry();
```

`start` returns `false` **without touching the incumbent** when the slot is
occupied. The registry is non-copyable and its destructor joins every
still-running job.

### The families

```cpp
// process.hpp
struct ProcessInfo { std::uint32_t pid = 0; std::string name; };
class IProcessControl {
public:
    virtual ~IProcessControl() = default;
    virtual std::vector<ProcessInfo> list() = 0;
    virtual std::uint32_t start(const std::vector<std::string>& argv) = 0;
    virtual void stop(std::uint32_t pid) = 0;
};
std::shared_ptr<IProcessControl> make_process_control();

// file.hpp
struct FileEntry { std::string name; std::uint64_t size = 0; bool is_directory = false; };
class IFileSystem {
public:
    virtual ~IFileSystem() = default;
    virtual std::vector<FileEntry> list(const std::string& path) = 0;
    virtual void remove(const std::string& path) = 0;
    virtual void move(const std::string& from, const std::string& to) = 0;
    virtual void copy(const std::string& from, const std::string& to) = 0;
};
std::shared_ptr<IFileSystem> make_file_system();

// power.hpp
class IPowerControl {
public:
    virtual ~IPowerControl() = default;
    virtual void shutdown() = 0;
    virtual void restart() = 0;
};
class ILogindBinding {
public:
    virtual ~ILogindBinding() = default;
    virtual bool available() = 0;
    virtual void power_off(bool restart) = 0;
};
class LogindPowerControl : public IPowerControl { /* ... */ };
class ExitWindowsPowerControl : public IPowerControl { /* ... */ };
std::shared_ptr<ILogindBinding> make_sd_bus_logind_binding();
std::shared_ptr<IPowerControl>  make_power_control();
std::shared_ptr<IPowerControl>  make_windows_power_control();
std::shared_ptr<IPowerControl>  make_linux_power_control(std::shared_ptr<ILogindBinding>);

// capture.hpp
class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;
    virtual Outcome::Deliverable grab() = 0;
};
std::shared_ptr<IScreenCapture> make_screen_capture();

// image_png.hpp
struct Frame { int width = 0; int height = 0; std::vector<unsigned char> bgr; };
std::vector<unsigned char> encode_png_bgr(const Frame& frame);
Frame decode_png(const std::vector<unsigned char>& png);
bool looks_like_png(const std::vector<unsigned char>& bytes);

// camera.hpp
class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual bool open() = 0;
    virtual bool read(Frame& out) = 0;
    virtual void close() = 0;
};
class ICameraCapture {
public:
    virtual ~ICameraCapture() = default;
    virtual void start_job() = 0;
    virtual Outcome::Deliverable stop_job() = 0;
};
class VideoCameraCapture : public ICameraCapture { /* ... */ };
std::shared_ptr<ICameraCapture> make_camera_capture();
std::shared_ptr<ICameraCapture> make_camera_capture_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry);
std::shared_ptr<IFrameSource> make_video_capture_source(int device_index);

// input.hpp
inline constexpr const char* kDefaultCaptureStorage = "keylog.txt";

struct InputEnvironment { bool input_capable = false; bool wayland = false; std::string note; };

class IInputEnvironmentProbe {
public:
    virtual ~IInputEnvironmentProbe() = default;
    virtual InputEnvironment probe() = 0;
};
class IKeyCaptureSink {
public:
    virtual ~IKeyCaptureSink() = default;
    virtual void append(const std::string& line) = 0;
    virtual JobDeliverable take_result() = 0;
    virtual void close() = 0;
};
class IKeyEventSource {
public:
    virtual ~IKeyEventSource() = default;
    virtual bool open() = 0;
    virtual std::vector<std::string> poll() = 0;
    virtual void close() = 0;
};
class IKeyboardGuard {
public:
    virtual ~IKeyboardGuard() = default;
    virtual void engage() = 0;
    virtual void release() = 0;
};
class IInputControl {
public:
    virtual ~IInputControl() = default;
    virtual void start_keylogger(const std::string& storage) = 0;
    virtual Outcome::Deliverable stop_keylogger() = 0;
    virtual void lock_keyboard() = 0;
    virtual void unlock_keyboard() = 0;
};
class InputControl final : public IInputControl { /* ... */ };
std::shared_ptr<IInputControl> make_input_control();
std::shared_ptr<IInputControl> make_input_control_with_registry(
    std::shared_ptr<BackgroundJobRegistry> registry);
std::shared_ptr<IKeyCaptureSink> make_file_capture_sink(const std::string& storage);

// app.hpp
struct AppEntry { std::string name; std::string path; };
class IAppLauncher {
public:
    virtual ~IAppLauncher() = default;
    virtual std::vector<AppEntry> list() = 0;
    virtual std::uint32_t start(const std::string& app_name) = 0;
};
std::shared_ptr<IAppLauncher> make_app_launcher();
std::shared_ptr<IAppLauncher> make_app_launcher_with_processes(
    std::shared_ptr<IProcessControl> processes);
std::shared_ptr<IAppLauncher> make_app_launcher_with_roots(
    std::vector<std::string> roots, std::shared_ptr<IProcessControl> processes);
std::vector<std::string> parse_desktop_exec(const std::string& exec_value);
```

---

## 5. `tether-cli`

`console/`.

```cpp
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnknownAgent = 3;
inline constexpr int kExitRefused = 4;

struct CliStreams {
    std::ostream& out;   // result bytes
    std::ostream& err;   // status/progress/errors
    std::istream& in;    // confirmation answers
    bool stdout_is_tty = true;
    bool stdin_is_tty = false;
};

enum class CancelOutcome { Sent, NotPending, NotEligible };

class IOperConsole {
public:
    virtual ~IOperConsole() = default;
    virtual std::vector<protocol::RegistryEntry> list_agents() = 0;
    virtual bool has_agent(const std::string& identity) = 0;
    virtual std::optional<protocol::CommandView> dispatch(
        const std::string& agent, const protocol::Cmd& command,
        const protocol::ResultHook& hook) = 0;
    virtual CancelOutcome cancel(const std::string& agent, std::uint32_t seq) = 0;
};

class CliSetupError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using ConsoleFactory =
    std::function<std::unique_ptr<IOperConsole>(const std::string& config_path)>;
using AuditStoreFactory =
    std::function<std::shared_ptr<AuditStore>(const std::string& config_path)>;

bool is_destructive_command(const std::string& command);

int run_cli(const std::vector<std::string>& arguments, CliStreams streams,
            const ConsoleFactory& make_console,
            const AuditStoreFactory& make_audit = nullptr);
int run_cli(int argc, const char* const argv[], CliStreams streams,
            const ConsoleFactory& make_console,
            const AuditStoreFactory& make_audit = nullptr);
```

`arguments[0]` is the program name and is skipped.

### Audit (`audit.hpp`)

```cpp
struct AuditRecord {
    std::string time;              // ISO-8601 UTC with milliseconds
    std::int64_t time_epoch_ms = 0;
    std::string agent;
    std::string command;
    std::vector<std::string> parameters;
    std::uint32_t seq = 0;         // 0 = nothing was dispatched
    std::string outcome;            // "executed" | "refused" | "failed"
    std::string record_type = "command";   // "command" | "job_event"
    std::string event;              // "started" | "stopped"
    std::string job;                // "camera" | "input"
    std::string delivered;          // the delivered capture file name
    std::string detail;
};

struct AuditQuery {
    std::optional<std::string> agent, command, outcome, record_type, event;
    std::optional<std::uint32_t> seq;
    std::optional<std::int64_t> since_ms, until_ms;
};

std::string classify_command_outcome(const std::string& error_code);
std::string format_audit_time_iso8601(std::int64_t epoch_ms);

struct JobEventHint { std::string job; std::string event; };
std::optional<JobEventHint> parse_job_event_line(const std::string& line);

class AuditStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AuditStore {
public:
    explicit AuditStore(const std::string& path = {});
    void append(const AuditRecord& record);
    std::vector<AuditRecord> query(const AuditQuery& query = {}) const;
    std::size_t size() const noexcept;
};

std::vector<AuditRecord> load_audit_jsonl(const std::string& path);
```

An empty `path` disables persistence (records stay queryable in memory only).
Construction **re-opens existing history**, so previously persisted records
are queryable again and appends continue after them. A write failure throws
`AuditStoreError` — the console reports it and exits non-zero rather than
continuing unaudited.

### TCP console (`tcp_console.hpp`)

```cpp
class TcpOperConsole : public IOperConsole {
public:
    explicit TcpOperConsole(std::unique_ptr<transport::ITransport> transport);
    void set_agent_dial_budget_ms(std::int64_t ms);
    // IOperConsole: list_agents, has_agent, dispatch, cancel
};
```

---

## 6. `tether-command-handler`

`agent/`.

```cpp
std::vector<protocol::Message> outcome_to_pieces(const syscalls::Outcome& outcome);

void install_commands(syscalls::CommandDispatcher& dispatcher,
                      protocol::AgentSession& session);
void install_commands(syscalls::CommandDispatcher& dispatcher,
                      tether::transport::TcpAgentLink& link);
```

`outcome_to_pieces` shaping rules:

| Outcome | Pieces |
| --- | --- |
| Failure | Exactly one `Err{code, message}`. |
| Success, text ≤ 128 KiB, no file | One `ResultText` (lines joined with `\n`, newline-terminated). |
| Success, with a file, or text > 128 KiB | `FileMeta{name,size}` + `FileChunk`(128 KiB each) + `FileEnd`. |
| Success, empty text, no file | The file channel with a synthesized `result.txt` deliverable. |

`dispatcher` must outlive the session.

---

## 7. Using the protocol layer directly

A minimal peer, in-memory, no sockets — this is what the protocol test suites
do:

```cpp
#include "session.hpp"

using namespace tether::protocol;

InMemoryConnection wires;
ConsoleSession console(wires.to_console, wires.to_agent,
                        SessionConfig{},
                        [](const std::string& name) {
                            return name == "lab-1" ? std::string("the-token")
                                                  : std::string();
                        });

AgentSession agent(wires.to_agent, wires.to_console,
                    AgentIdentity{"lab-1", "lab-1-001"},
                    "the-token", SessionConfig{});
agent.set_command_handler([](std::uint32_t seq, const Cmd& cmd,
                              const std::function<bool()>& is_cancelled) {
    // ... run the capability, then:
    // agent.send_result(seq, ResultText{"done\n"});
});

agent.begin(0);
console.pump(1);   // console sees HELLO, sends the CHALLENGE
agent.pump(2);     // agent answers
console.pump(3);   // console sends AUTH_OK
agent.pump(4);     // agent reaches Established
```

Two details to copy: **time is injected** (never call a clock), and the
console's token resolver returns an **empty string** for an unknown identity,
which is how the console refuses it before issuing any challenge.
