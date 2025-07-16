#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <sstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <iterator>

void setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

// Разбивает строку выражений через пробел
std::vector<std::string> string_parser(const std::string& expr) {
    std::string str;
    std::vector<std::string> strings;
    for (const char& x: expr){
        if (x == ' ') {
            strings.push_back(str);
            str = "";
        } else {
            str.push_back(x);
        }
    }
    if (!str.empty()) {
        strings.push_back(str);
    }
    return strings;
}
// Парсер одного простого выражения без скобок
float math_parser(const std::string& str) {
    std::istringstream ss(str);
    std::vector<float> numbers;
    std::vector<char> operators;
    char op;
    float number;
    ss >> number;
    numbers.push_back(number);
    while (ss >> op >> number) {
        operators.push_back(op);
        numbers.push_back(number);
    }
    for (int i = 0; i < operators.size(); ++i) {
        if (operators[i] == '*' || operators[i] == '/') {
            float result = operators[i] == '*' ? numbers[i] * numbers[i+1] : numbers[i] / numbers[i+1];
            numbers.erase(numbers.begin() + i);
            numbers.erase(numbers.begin() + i);
            operators.erase(operators.begin() + i);
            numbers.insert(numbers.begin() + i, result);
            --i;
        }
    }
    float result = numbers[0];
    for (int i = 0; i < operators.size(); ++i) {
        result = operators[i] == '+' ? result + numbers[i+1] : result - numbers[i+1];
    }
    return result;
}
// Оценивает строку, содержащую одно или несколько выражений через пробел
std::vector<float> evaluateExpression(const std::string& expr) {
    std::vector<std::string> strings = string_parser(expr);
    std::vector<float> result_vector;
    for (const std::string& str: strings) {
        result_vector.push_back(math_parser(str));
    }
    return result_vector;
}

// Генерация строки от 1 до 3 выражений, каждое из n чисел
std::string generateExpression(int n) {
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> expr_count(1, 3);
    std::uniform_int_distribution<> num_dist(1, 100);
    std::uniform_int_distribution<> op_dist(0, 3);
    const char ops[] = {'+', '-', '*', '/'};
    std::ostringstream oss;
    int count = expr_count(gen);
    for (int e = 0; e < count; ++e) {
        for (int i = 0; i < n; ++i) {
            oss << num_dist(gen);
            if (i < n - 1) oss << ops[op_dist(gen)];
        }
        if (e + 1 < count) oss << ' ';
    }
    return oss.str();
}

// Нарезка строки на случайные фрагменты
std::vector<std::string> splitRandomChunks(const std::string& str, int max_chunk = 4) {
    static std::mt19937 gen(std::random_device{}());
    int len = str.size();
    std::uniform_int_distribution<> dist(1, std::max(1, len / max_chunk));
    std::vector<std::string> chunks;
    size_t pos = 0;
    while (pos < str.size()) {
        int chunk_len = dist(gen);
        chunks.push_back(str.substr(pos, chunk_len));
        pos += chunk_len;
    }
    return chunks;
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: ./tcp_client_verifier <n> <connections> <server_addr> <server_port>\n";
        return 1;
    }
    int n = std::stoi(argv[1]);
    int connections = std::stoi(argv[2]);
    std::string server_addr = argv[3];
    int server_port = std::stoi(argv[4]);

    // Устанавливаем сокеты
    std::vector<int> sockets;
    for (int i = 0; i < connections; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { perror("socket"); return 1; }
        sockaddr_in serv{};
        serv.sin_family = AF_INET;
        serv.sin_port = htons(server_port);
        inet_pton(AF_INET, server_addr.c_str(), &serv.sin_addr);
        if (connect(sock, (sockaddr*)&serv, sizeof(serv)) < 0) {
            perror("connect"); close(sock); return 1;
        }
        setNonBlocking(sock);
        sockets.push_back(sock);
    }

    int epfd = epoll_create1(0);
    for (int sock : sockets) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = sock;
        epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
    }

    struct Job {
        std::string line;
        std::vector<float> expected;
        std::vector<std::string> chunks;
        bool sent = false;
    };
    std::vector<Job> jobs;
    // Подготавливаем задачи
    for (int i = 0; i < connections; ++i) {
        Job job;
        job.line = generateExpression(n);
        job.expected = evaluateExpression(job.line);
        job.chunks = splitRandomChunks(job.line);
        jobs.push_back(std::move(job));
    }

    std::unordered_map<int, std::string> recv_buf;
    int completed = 0;
    epoll_event events[16];

    while (completed < connections) {
        int nfds = epoll_wait(epfd, events, 16, -1);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            int idx = std::distance(sockets.begin(), std::find(sockets.begin(), sockets.end(), fd));
            auto & job = jobs[idx];

            if (!job.sent && (events[i].events & EPOLLOUT)) {
                // Шлём фрагменты и маркер конца
                for (auto & c : job.chunks) send(fd, c.c_str(), c.size(), 0);
                send(fd, "\n", 1, 0);
                job.sent = true;
            }

            if (job.sent && (events[i].events & EPOLLIN)) {
                char buf[1024];
                ssize_t len = read(fd, buf, sizeof(buf));
                if (len > 0) {
                    recv_buf[fd].append(buf, len);
                    auto pos = recv_buf[fd].find('\n');
                    if (pos != std::string::npos) {
                        std::string resp = recv_buf[fd].substr(0, pos);
                        auto actual = evaluateExpression(resp);
                        bool ok = (actual.size() == job.expected.size());
                        if (ok) {
                            for (size_t j = 0; j < actual.size(); ++j) {
                                if (std::fabs(actual[j] - job.expected[j]) > 1e-3) { ok = false; break; }
                            }
                        }
                        if (ok) std::cout << "✅ OK: " << job.line << std::endl;
                        else {
                            std::cerr << "❌ Incorrect\nSent:     " << job.line
                                      << "\nExpected: ";
                            for (auto v : job.expected) std::cerr << v << ' ';
                            std::cerr << "\nReceived: " << resp << std::endl;
                        }
                        close(fd);
                        completed++;
                    }
                }
            }
        }
    }
    close(epfd);
    return 0;
}

