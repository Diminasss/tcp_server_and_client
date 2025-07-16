#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <iomanip>

void setNonBlocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
}

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

std::vector<float> evaluateExpression(const std::string& expr) {
    std::vector<std::string> strings = string_parser(expr);
    std::vector<float> result_vector;
    for (const std::string& str: strings) {
        result_vector.push_back(math_parser(str));
    }
    return result_vector;
}

struct ClientState {
    std::string recv_buffer;
    std::string send_buffer;
    size_t send_offset = 0;
};

int main(int argc, char* argv[]) {
    for (float x: evaluateExpression("2*2*2+2*2/2 2*2*2/2+2 2+2+2-3*2")){std::cout<<x<<" ";}
    if (argc != 2) {
        std::cerr << "Usage: ./tcp_calc_server <port>\n";
        return 1;
    }

    int port = std::stoi(argv[1]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    setNonBlocking(server_fd);
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }

    epoll_event ev{}, events[64];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    std::unordered_map<int, ClientState> clients;
    std::cout << "Server started on port " << port << std::endl;

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, 64, -1);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (fd == server_fd) {
                sockaddr_in client_addr{};
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }
                setNonBlocking(client_fd);
                ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                clients[client_fd] = ClientState{};
            } else {
                if (events[i].events & EPOLLIN) {
                    char buf[1024];
                    ssize_t count = read(fd, buf, sizeof(buf));
                    if (count <= 0) {
                        close(fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                        clients.erase(fd);
                        continue;
                    }
                    ClientState& state = clients[fd];
                    state.recv_buffer.append(buf, count);

                    // Обработка всех выражений разом
                    try {
                        std::vector<float> result = evaluateExpression(state.recv_buffer);
                        std::ostringstream response;
                        response << std::fixed << std::setprecision(6);
                        for (float val : result) response << val << " ";
                        response << "\n";
                        state.send_buffer += response.str();
                        state.recv_buffer.clear();
                    } catch (const std::exception& e) {
                        state.send_buffer += std::string("Error: ") + e.what() + "\n";
                        state.recv_buffer.clear();
                    }
                }

                if (events[i].events & EPOLLOUT) {
                    ClientState& state = clients[fd];
                    while (state.send_offset < state.send_buffer.size()) {
                        ssize_t sent = send(fd, state.send_buffer.data() + state.send_offset,
                                            state.send_buffer.size() - state.send_offset,
                                            MSG_NOSIGNAL);
                        if (sent < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            close(fd);
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                            clients.erase(fd);
                            break;
                        }
                        state.send_offset += sent;
                    }
                    if (state.send_offset == state.send_buffer.size()) {
                        state.send_buffer.clear();
                        state.send_offset = 0;
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
