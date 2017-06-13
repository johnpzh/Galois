#include <iostream>
#include <string>
#include <fstream>

class FileReader {
  char *buffer, *bufferEnd, *cursor;
  size_t fileSize;
  char *delimiters;
  size_t numDelimiters;
  char *separators;
  size_t numSeparators;

private:
  bool isSeparator(char c);
  bool isDelimiter(char c);

public:
  FileReader(std::string inName, 
    char *delimiters, size_t numDelimiters, 
    char *separators, size_t numSeparators);
  ~FileReader();
  std::string nextToken();
};

