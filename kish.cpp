#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

// TODO LIST:
// Make 200 lines from the current 400+
// Check the $-code for 1) that it does not start with rm and 2) it compiles
// - use some bash linter for that
// Fix the editor in KI|asdasd|SH - it is able to edit the entire screen now
// Add the quote screening for sending queries containing them
// Nvim history contains a lot of bs if open nvim
// Handle API errors(rate limits) graciously
// Make multiple threads to be able to export history while you wait
// Add Ctrl-C to stop the process
// Add some marker to see that the model even works/thinks

std::string homedir;
void load_env() {
  char *c = getenv("HOME");
  homedir = c ? c : std::string(getpwuid(getuid())->pw_dir);

  std::string env_path = homedir + "/.env";
  std::ifstream env_file(env_path);
  if (!env_file) {
    return; // .env file not found, not an error
  }

  std::string line;
  while (std::getline(env_file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t pos = line.find('=');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string value = line.substr(pos + 1);
      setenv(key.c_str(), value.c_str(), 1);
    }
  }
}

std::string escape_json_string(std::string_view s) {
  std::string res;
  res.reserve(s.length());
  for (char c : s) {
    switch (c) {
    case '"':
      res += "\\\"";
      break;
    case '\\':
      res += "\\\\";
      break;
    case '\b':
      res += "\\b";
      break;
    case '\f':
      res += "\\f";
      break;
    case '\n':
      res += "\\n";
      break;
    case '\r':
      res += "\\r";
      break;
    case '\t':
      res += "\\t";
      break;
    default:
      res += c;
      break;
    }
  }
  return res;
}

std::string strip_ansi(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    if (input[i] == '\x1b') {
      i++;
      if (i >= input.size())
        break;

      if (input[i] == '[') { // CSI
        i++;
        // Parameters
        while (i < input.size() && input[i] >= 0x30 && input[i] <= 0x3F) {
          i++;
        }
        // Intermediate bytes
        while (i < input.size() && input[i] >= 0x20 && input[i] <= 0x2F) {
          i++;
        }
        // Final byte
        if (i < input.size() && input[i] >= 0x40 && input[i] <= 0x7E) {
          i++;
        }
      } else if (input[i] == ']') { // OSC
        i++;
        // Terminated by BEL (0x07) or ST (ESC \)
        size_t end = input.find('\x07', i);
        if (end != std::string_view::npos) {
          i = end + 1;
        } else {
          end = input.find("\x1b\\", i);
          if (end != std::string_view::npos) {
            i = end + 2;
          } else {
            // Not terminated, assume it runs to the end of the buffer.
            i = input.size();
          }
        }
      } else {
        // Other 2-char escape sequences.
        i++;
      }
    } else {
      if (isprint(static_cast<unsigned char>(input[i])) || input[i] == '\n' ||
          input[i] == '\r' || input[i] == '\t') {
        output += input[i];
      }
      i++;
    }
  }
  return output;
}

#include <cstdio>
#include <vector>

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

void send_prompt(int master_fd, std::vector<std::string> &history) {
  // write(master_fd, "\n", 1);
  std::string kishrc_path = homedir + "/.kishrc";
  std::string history_path = homedir + "/.kish_history";

  std::ifstream kishrc_file(kishrc_path);
  std::string kishrc((std::istreambuf_iterator<char>(kishrc_file)),
                     std::istreambuf_iterator<char>());
  if (kishrc.empty()) {
    std::cerr << "\nNo kishrc found. Please create ~/.kishrc" << std::endl;
    return;
  }
  std::string prompt =
      std::accumulate(history.begin() + 1, history.end(), history[0]);

  std::string escaped_kishrc = escape_json_string(kishrc);
  std::string escaped_prompt = escape_json_string(prompt);

  std::ofstream(history_path) << prompt;

  const char *api_key_cstr = getenv("MOONSHOT_API_KEY");
  if (!api_key_cstr) {
    std::cerr
        << "\nError: MOONSHOT_API_KEY environment variable not set. You can "
           "write it like MOONSHOT_API_KEY=sk-... in your ~/.env file."
        << std::endl;
    return;
  }
  std::string kimi_api_key = api_key_cstr;

  std::string json_payload = R"({
    "model": "kimi-k2-thinking-turbo",
    "messages": [
        {"role": "system", "content": ")" +
                             escaped_kishrc + R"("},
        {"role": "user", "content": ")" +
                             escaped_prompt + R"("}
    ],
    "temperature": 0.5
  })";

  // Write JSON payload to a temporary file to avoid shell injection
  std::string payload_temp_file_path = "/tmp/kish_payload.json";
  std::ofstream payload_temp_file(payload_temp_file_path);
  if (!payload_temp_file) {
    std::cerr << "\nError: Could not create temporary file for JSON payload."
              << std::flush;
    return;
  }
  payload_temp_file << json_payload;
  payload_temp_file.close();

  std::string command_base =
      "curl -s https://api.moonshot.ai/v1/chat/completions "
      "-H \"Content-Type: application/json\" "
      "-H \"Authorization: Bearer " +
      kimi_api_key +
      "\" "
      "-d @" +
      payload_temp_file_path + " 2>&1";

  try {
    // std::cout << "Sending" << command_base;
    std::string raw_response = exec(command_base.c_str());
    // std::cout << "Received" << raw_response;

    // Write response to a temporary file to avoid shell injection
    std::string temp_file_path = "/tmp/kish_response.json";
    std::ofstream temp_file(temp_file_path);
    if (!temp_file) {
      std::cerr << "\nError: Could not create temporary file for response."
                << std::flush;
      remove(payload_temp_file_path.c_str()); // Clean up payload file
      return;
    }
    temp_file << raw_response;
    temp_file.close();

    // Check for top-level API errors first
    std::string error_check_command =
        "jq -r 'if .error then .error.message else \"\" end' " + temp_file_path;
    std::string api_error = exec(error_check_command.c_str());

    if (!api_error.empty() && api_error != "null\n" &&
        !api_error.starts_with("\n")) {
      std::cerr << "\nAPI Error: " << api_error << std::flush;
      history.push_back("API Error: " + api_error);
      remove(temp_file_path.c_str());         // Clean up response file
      remove(payload_temp_file_path.c_str()); // Clean up payload file
      return;
    }

    // Extract content
    std::string content_command =
        "jq -r '.choices[0].message.content' " + temp_file_path;
    std::string content = exec(content_command.c_str());

    // Extract reasoning content
    std::string reasoning_command =
        "jq -r '.choices[0].message.reasoning_content' " + temp_file_path;
    std::string reasoning_content = exec(reasoning_command.c_str());

    // Clean up the temporary files
    remove(temp_file_path.c_str());
    remove(payload_temp_file_path.c_str());

    if (!content.empty() && content != "null\n") {
      if (content.starts_with("$ ")) {
        std::string command_to_run = content.substr(2);
        // The model might add a newline, let's strip it.
        if (!command_to_run.empty() && command_to_run.back() == '\n') {
          command_to_run.pop_back();
        }
        history.push_back(command_to_run);
        std::string exec_str = command_to_run + "\nKISH_CMD_DONE";
        write(master_fd, exec_str.c_str(), exec_str.length());
      } else {
        std::cout << "\n" << content << std::flush;

        write(master_fd, "\n", 1);
        history.push_back(content);
      }
    } else {
      std::cerr << "\nCould not parse AI response content: " << raw_response
                << std::flush;
      history.push_back("Could not parse AI response content: " + raw_response);
    }
    history.push_back("\n<thoughts>\n" + reasoning_content + "\n</thoughts>\n");

  } catch (const std::runtime_error &e) {
    std::cerr << "\nError executing curl/jq: " << e.what() << std::flush;
  }
}

int main() {
  load_env();

  struct termios orig_termios;
  tcgetattr(STDIN_FILENO, &orig_termios);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  int master_fd;
  pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);

  if (pid < 0) {
    std::cerr << "forkpty: " << strerror(errno) << std::endl;
    return 1;
  } else if (pid == 0) {
    execlp("/bin/bash", "/bin/bash", NULL);
    exit(1);
  } else {
    // Parent process
    std::vector<std::string> history;
    std::string current_input;
    bool kish_mode = false;

    while (true) {
      struct pollfd fds[2];
      fds[0].fd = STDIN_FILENO;
      fds[0].events = POLLIN;
      fds[1].fd = master_fd;
      fds[1].events = POLLIN;

      int ret = poll(fds, 2, -1);
      if (ret < 0) {
        std::cerr << "poll: " << strerror(errno) << std::endl;
        break;
      }

      if (fds[0].revents & POLLIN) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0)
          break;

        if (c == 24) { // Ctrl+X
          kish_mode = !kish_mode;
          if (kish_mode) {
            std::cout << "KI|" << std::flush;
          } else {
            std::cout << "|SH" << std::flush;
            history.push_back("KI|" + current_input + "|SH");
            send_prompt(master_fd, history);
            current_input.clear();
          }
          continue;
        } else if (c == 8) { // Ctrl+H
          std::ofstream output_history_file(homedir + "/kish_history.txt");
          if (output_history_file.is_open()) {
            for (const auto &entry : history) {
              output_history_file << entry << "\n";
            }
            output_history_file.close();
            std::cout << "\nHistory exported to ~/kish_history.txt"
                      << std::flush;
          } else {
            std::cerr << "\nError: Could not open ~/history.txt for writing."
                      << std::flush;
          }
          continue;
        }

        if (kish_mode) {
          if (c == '\r' || c == '\n') {
            history.push_back(current_input);
            current_input.clear();
            std::cout << "\nKI|" << std::flush;
          } else if (c == 127) { // Backspace
            if (!current_input.empty()) {
              current_input.pop_back();
              std::cout << "\b \b" << std::flush;
            }
          } else {
            current_input += c;
            std::cout << c << std::flush;
          }
        } else {
          write(master_fd, &c, 1);
        }
      }

      if (fds[1].revents & POLLIN) {
        std::array<char, 1024> buffer;
        ssize_t count = read(master_fd, buffer.data(), buffer.size());
        if (count <= 0)
          break;

        std::string shell_output(buffer.data(), count);
        size_t marker_pos = shell_output.find("KISH_CMD_DONE");
        if (marker_pos != std::string::npos) {
          // Command finished, process output before marker
          std::string command_output = shell_output.substr(0, marker_pos);
          write(STDOUT_FILENO, command_output.c_str(), command_output.length());
          history.push_back(strip_ansi(command_output));
          send_prompt(master_fd, history);
          // Any remaining output after marker (e.g., new prompt) will be
          // handled in next poll
        } else {
          write(STDOUT_FILENO, buffer.data(), count);

          history.push_back(
              strip_ansi({buffer.data(), static_cast<size_t>(count)}));
        }
      }
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    waitpid(pid, NULL, 0);
  }

  return 0;
}
