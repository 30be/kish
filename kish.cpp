// TODO LIST:
// - Make multiple threads to be able to export history while you wait
// - Add Ctrl-C to stop the process
// - Add some marker to see that the model even works/thinks
// - Parse the thoughts to send them in the js separately
// - Quite a bit of prompt engineering

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <numeric>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;

struct log {
  template <class T> log operator<<(const std::span<T> span) {
    std::ofstream l("/tmp/kish_log", std::ios::app);
    l << "span{size=" << span.size() << ",data=[";
    for (auto &el : span)
      if (isprint(el))
        l << el;
      else
        l << "(" << int(el) << ")";
    l << "]}";
    return *this;
  }
  template <class T> log &operator<<(const T &val) {
    std::ofstream("/tmp/kish_log", std::ios::app) << val;
    return *this;
  }
} log;

std::string homedir;
std::vector<std::string> history;
std::string current_input;
bool kish_mode = false;
int master_fd;

int CTRL_H = 8;
int CTRL_X = 24;
char ESC = '\x1b';
char BEL = 7;
int DELETE = 127;
int BACKSPACE = 8;
int CARRIAGE_RETURN = 13;
int API_RETRIES = 5;
const std::string ALTERNATE_SCREEN_SEQUENCE = "\x1b[?1049h";
const std::string ALTERNATE_SCREEN_RETURN_SEQUENCE = "\x1b[?1049l";

void load_env() {
  char *c = getenv("HOME");
  homedir = c ? c : std::string(getpwuid(getuid())->pw_dir);

  std::string env_path = homedir + "/.env";
  std::ifstream env_file(env_path);
  if (!env_file)
    return; // .env file not found, not an error

  std::string line;
  while (std::getline(env_file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    size_t pos = line.find('=');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string value = line.substr(pos + 1);
      setenv(key.c_str(), value.c_str(), 1);
    }
  }
}

std::string escape_json(const std::string &s) {
  auto get_escaped_char = [](auto c) -> std::string {
    switch (c) {
    case '"': return "\\\"";
    case '\\': return "\\\\";
    case '\b': return "\\b";
    case '\f': return "\\f";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: return {c};
    }
  };
  return std::accumulate(
      s.begin(), s.end(), std::string(),
      [&](std::string res, char c) { return res + get_escaped_char(c); });
}

// Function to execute a command and return its output
std::string exec(const char *cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

std::string cleanup_history(std::string history) {
  std::string res;
  for (int i = 0; i < history.length(); i++)
    if (ALTERNATE_SCREEN_SEQUENCE ==
        history.substr(i, ALTERNATE_SCREEN_RETURN_SEQUENCE.length())) {
      while (history.substr(i, ALTERNATE_SCREEN_RETURN_SEQUENCE.length()) !=
             ALTERNATE_SCREEN_RETURN_SEQUENCE)
        i++;
      i += ALTERNATE_SCREEN_RETURN_SEQUENCE.length() - 1;
    } else if (history[i] == ESC && i < history.length() - 1 &&
               history[i + 1] == '[') {
      while (!isalpha(history[i]))
        i++;
    } else if (history[i] == ESC && i < history.length() - 1 &&
               history[i + 1] == ']')
      while (history[i] != BEL) {
        if (history[i] == ESC && i < history.length() - 1 &&
            history[i + 1] == '\\') {
          i++;
          break;
        }
        i++;
      }
    else if (history[i] == BACKSPACE)
      res.pop_back();
    else if (history[i] == CARRIAGE_RETURN)
      ;
    else
      res += history[i];
  return res;
}
std::optional<std::pair<std::string, std::string>> get_response() {
  std::string kishrc_path = homedir + "/.kishrc";
  std::string history_path = homedir + "/.kish_history";

  std::ifstream kishrc_file(kishrc_path);
  std::string kishrc((std::istreambuf_iterator<char>(kishrc_file)),
                     std::istreambuf_iterator<char>());
  if (kishrc.empty()) {
    std::cerr << "\nNo kishrc found. Please create ~/.kishrc" << std::endl;
    return {};
  }
  std::string prompt =
      std::accumulate(history.begin() + 1, history.end(), history[0]);
  prompt = cleanup_history(prompt);

  std::ofstream(history_path) << prompt;

  const char *api_key_cstr = getenv("MOONSHOT_API_KEY");
  if (!api_key_cstr) {
    std::cerr
        << "\nError: MOONSHOT_API_KEY environment variable not set. You can "
           "write it like MOONSHOT_API_KEY=sk-... in your ~/.env file."
        << std::endl;
    return {};
  }

  std::string json_payload =
      std::format(R"({{
    "model": "kimi-k2-thinking-turbo",
    "messages": [
        {{"role": "system", "content": "{}"}},
        {{"role": "user", "content": "{}"}}
    ],
    "temperature": 0.5
}})",
                  escape_json(kishrc), escape_json(prompt));
  std::string payload_temp_file_path = "/tmp/kish_payload.json";
  std::ofstream(payload_temp_file_path) << json_payload;

  std::string command_base =
      std::format("curl -s https://api.moonshot.ai/v1/chat/completions "
                  "-H \"Content-Type: application/json\" "
                  "-H \"Authorization: Bearer {}\" "
                  "-d @{} 2>&1",
                  api_key_cstr, payload_temp_file_path);
  try {
    for (int i = 0; i < API_RETRIES; i++) {
      std::string raw_response = exec(command_base.c_str());
      std::string temp_file_path = "/tmp/kish_response.json";
      std::ofstream(temp_file_path) << raw_response;

      std::string error_check_command =
          "jq -r 'if .error then .error.message else \"\" end' " +
          temp_file_path;
      std::string api_error = exec(error_check_command.c_str());

      if (!api_error.empty() && api_error != "\n") {
        std::cerr << "\nAPI Error: " << api_error << "\nRetrying..."
                  << std::flush;
        history.push_back("API Error: " + api_error + "\nRetrying...");
        std::this_thread::sleep_for(5s);
        continue; // GOTO while(true)
      }

      std::string content_command =
          "jq -r '.choices[0].message.content' " + temp_file_path;
      std::string content = exec(content_command.c_str());

      std::string reasoning_command =
          "jq -r '.choices[0].message.reasoning_content' " + temp_file_path;
      std::string reasoning_content = exec(reasoning_command.c_str());

      fs::remove(temp_file_path);
      fs::remove(payload_temp_file_path);
      if (content.empty()) {
        std::cerr << "\nCouldn't parse response:" << raw_response << std::flush;
        history.push_back("Could not parse AI response content: " +
                          raw_response);
        return {};
      }
      return {{content, reasoning_content}};
    }
    return {};
  } catch (const std::runtime_error &e) {
    std::cerr << "\nError executing curl/jq: " << e.what() << std::flush;
    return {};
  }
}
void send_prompt() {
  auto response = get_response();
  if (!response)
    return;

  auto [content, reasoning_content] = *response;

  if (content.starts_with("$ ")) {
    std::string command_to_run = content.substr(2);
    if (!command_to_run.empty() && command_to_run.back() == '\n')
      command_to_run.pop_back();
    auto err = exec(("bash -nc " + command_to_run).c_str());
    history.push_back(command_to_run);
    std::string exec_str = command_to_run + "\nKISH_CMD_DONE";
    if (!err.empty())
      history.push_back(err);
    else
      write(master_fd, exec_str.c_str(), exec_str.length());
  } else {
    std::cout << content << std::flush;
    write(master_fd, "\n\n", 1);
    history.push_back(content);
  }
  history.push_back("\n<thoughts>\n" + reasoning_content + "\n</thoughts>\n");
}

void save_history() {
  std::string prompt =
      std::accumulate(history.begin() + 1, history.end(), history[0]);
  prompt = cleanup_history(prompt);
  std::ofstream(homedir + "/kish_history.txt") << prompt;
  std::cout << "\nHistory exported to ~/kish_history.txt" << std::flush;
}

void read_keyboard(std::span<char> data) {
  log << data;
  if (data.empty())
    return;
  char c = data[0];
  if (c == CTRL_X) {
    log << "Ctrl-X detected\n";
    kish_mode = !kish_mode;
    if (kish_mode) {
      std::cout << "KI|" << std::flush;
    } else {
      std::cout << "|SH" << std::endl;
      history.push_back("KI|" + current_input + "|SH");
      send_prompt();
      current_input.clear();
    }
  } else if (c == CTRL_H) {
    log << "Ctrl-H detected\n";
    save_history();
  } else if (kish_mode) {
    if (c == '\r' || c == '\n') {
      history.push_back(current_input);
      current_input.clear();
      std::cout << "\nKI|" << std::flush;
    } else if (c == DELETE) {
      if (!current_input.empty()) {
        current_input.pop_back();
        std::cout << "\b \b" << std::flush;
      }
    } else if (std::isprint(c)) {
      current_input += c;
      std::cout << c << std::flush;
    } // Just ignore everything else(arrows not supported)
  } else {
    write(master_fd, data.data(), data.size());
    return;
  }
  read_keyboard(data.subspan(1));
}

void read_terminal(std::span<char> data) {
  std::string shell_output(data.begin(), data.end());
  size_t marker_pos = shell_output.find("KISH_CMD_DONE");
  if (marker_pos != std::string::npos) {
    // Command finished, process output before marker
    std::string command_output = shell_output.substr(0, marker_pos);
    write(STDOUT_FILENO, command_output.c_str(), command_output.length());
    history.push_back(command_output);
    send_prompt();
    // Any remaining output after marker (e.g., new prompt) will be
    // handled in next poll
  } else {
    log << "Got shell_output, writing:" << data << "\n";
    write(STDOUT_FILENO, data.data(), data.size());
    history.push_back({data.begin(), data.end()});
  }
}

void run() {
  while (true) {
    struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {master_fd, POLLIN, 0}};
    int ret = poll(fds, 2, -1);
    if (ret < 0) {
      std::cerr << "poll: " << strerror(errno) << std::endl;
      break;
    }

    for (int fd : {0, 1})
      if (fds[fd].revents & POLLIN) {
        std::array<char, 1024> buffer;
        ssize_t count = read(fds[fd].fd, buffer.data(), buffer.size());
        if (count <= 0)
          break;
        std::array<std::function<void(std::span<char>)>, 2>{read_keyboard,
                                                            read_terminal}[fd](
            std::span<char>(buffer.data(), static_cast<size_t>(count)));
      }
  }
}

int main() {
  load_env();

  struct termios orig_termios;
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);

  if (pid < 0) {
    std::cerr << "forkpty: " << strerror(errno) << std::endl;
    return 1;
  } else if (pid == 0) {
    execlp("/bin/bash", "/bin/bash", NULL);
    return 1;
  }
  run();
  // Restore terminal settings
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
  waitpid(pid, NULL, 0);

  return 0;
}
