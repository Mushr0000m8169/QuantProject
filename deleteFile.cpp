#include <iostream>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;


int main() {
    // 1. Get the folder path and filename from the user
    std::string folder_input;
    std::string file_input;

    std::cout << "Enter the absolute or relative folder path: ";
    std::getline(std::cin, folder_input);

    std::cout << "Enter the specific name of the file to delete (with extension): ";
    std::getline(std::cin, file_input);

    // 2. Combine them into a secure, cross-platform filesystem path object
    fs::path folder_path(folder_input);
    fs::path file_path = folder_path / file_input; // The '/' operator automatically appends correct system separators

    // 3. Verify if the file exists and is a regular file before attempting deletion
    if (!fs::exists(file_path)) {
        std::cerr << "Error: The file does not exist at " << file_path << "\n";
        return 1;
    }
    
    if (fs::is_directory(file_path)) {
        std::cerr << "Error: The target name is a directory, not a file.\n";
        return 1;
    }

    // 4. Attempt to delete the file safely using an error code object to prevent crashing
    std::error_code ec;
    bool deleted = fs::remove(file_path, ec);

    if (deleted) {
        std::cout << "Success: \"" << file_input << "\" was successfully deleted.\n";
    } else {
        std::cerr << "Error: Could not delete the file. Reason: " << ec.message() << "\n";
    }

    return 0;
}