cmake_minimum_required(VERSION 3.25)
project(TCP_Project)

set(CMAKE_CXX_STANDARD 23)

# Добавляем подпроекты
add_subdirectory(Server)
add_subdirectory(Client)