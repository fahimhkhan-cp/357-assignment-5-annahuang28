#define _GNU_SOURCE
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

// use getline
// your server will accept a request (details below) for a file,
// read that file, and send the file contents back to the client.

#define MAX_LINE 1024

// send HTTP response
void send_request(int nfd, int status_code, const char *status_message, const char *content_type, const char *body, size_t body_size) {
    // Send the HTTP Header
    dprintf(nfd, "HTTP/1.0 %d %s\r\n", status_code, status_message);
    dprintf(nfd, "Content-Type: %s\r\n", content_type);
    dprintf(nfd, "Content-Length: %zu\r\n", body_size);
    dprintf(nfd, "\r\n");

    // body if it exists
    if (body != NULL && body_size > 0) {
        write(nfd, body, body_size);
    }
}

// handle HTTP request
void handle_request(int nfd) {
   FILE *network = fdopen(nfd, "r");
   char *line = NULL;
   size_t size = 0;
   ssize_t num;
   char method[MAX_LINE], filename[MAX_LINE], version[MAX_LINE];

   if (network == NULL) {
      perror("fdopen");
      close(nfd);
      return;
   }

   // read the first line of the HTTP request
   num = getline(&line, &size, network);
   if (num < 0) {
      perror("getline");
      free(line);
      fclose(network);
      return;
   }
   // requirement 4: Erroneous requests will be responded to with an appropriate error response
   // parse the request line (method, filename, and version)
   if (sscanf(line, "%s %s %s", method, filename, version) != 3) {
      send_request(nfd, 400, "Bad Request", "text/html", "<html><body><h1>400 Bad Request</h1></body></html>", 50);
      free(line);
      fclose(network);
      return;
   }

   // strip leading '/' from filename and check for '..' to prevent directory traversal
   if (filename[0] == '/') {
      memmove(filename, filename + 1, strlen(filename));
   }

   if (strstr(filename, "..") != NULL) {
      send_request(nfd, 403, "Forbidden", "text/html", "<html><body><h1>403 Forbidden</h1></body></html>", 51);
      free(line);
      fclose(network);
      return;
   }

   // requirement 3: support only two of the HTTP request types: HEAD and GET
   if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
      struct stat st;
      if (stat(filename, &st) == -1) {
         // for file not found
         send_request(nfd, 404, "Not Found", "text/html", "<html><body><h1>404 Not Found</h1></body></html>", 46);
      } else if (S_ISDIR(st.st_mode)) {
         // directory request (forbidden)
         send_request(nfd, 403, "Forbidden", "text/html", "<html><body><h1>403 Forbidden</h1></body></html>", 51);
      } else {
         // file found, prepare to send the content
         FILE *file = fopen(filename, "r");
         if (file == NULL) {
            send_request(nfd, 500, "Internal Error", "text/html", "<html><body><h1>500 Internal Error</h1></body></html>", 49);
         } else {
            // read file content if GET request, otherwise just send headers for HEAD
            char *body = NULL;
            size_t body_size = 0;
            if (strcmp(method, "GET") == 0) {
               // Read the entire file into memory
               fseek(file, 0, SEEK_END);
               body_size = ftell(file);
               fseek(file, 0, SEEK_SET);
               body = malloc(body_size);
               fread(body, 1, body_size, file);
            }

            // send HTTP response (with body only for GET)
            send_request(nfd, 200, "OK", "text/html", body, body_size);

            // clean up
            if (body) free(body);
            fclose(file);
         }
      }
   } else if (strncmp(method, "GET /cgi-like", 13) == 0) {
      // requirement 5 cgi-like Support
      char command[MAX_LINE];
      snprintf(command, sizeof(command), ".%s", filename); 
      char *args = strchr(filename, '?');
      if (args != NULL) {
         // ignore args for now in the command
         *args = '\0';
      }

      //
      if (strncmp(command, "./cgi-like/", 11) != 0) {
         send_request(nfd, 403, "Forbidden", "text/html", "<html><body><h1>403 Forbidden</h1></body></html>", 51);
      } else {
         // forks a child to execute the CGI program
         pid_t pid = fork();
         if (pid == 0) { // child
            // executes the CGI program and redirect output to a temporary file
            char temp_file[MAX_LINE];
            snprintf(temp_file, sizeof(temp_file), "/tmp/output_%d.txt", getpid());
            FILE *temp_fp = fopen(temp_file, "w");
            if (!temp_fp) {
               send_request(nfd, 500, "Internal Error", "text/html", "<html><body><h1>500 Internal Error</h1></body></html>", 49);
               exit(1);
            }
            fclose(temp_fp);

            // execute the command
            execlp(command, command, NULL);
            exit(1);  // if execlp fails
         } else if (pid > 0) { // parent
            wait(NULL); // wait for the child to finish

            // read the output file and send response
            char temp_file[MAX_LINE];
            snprintf(temp_file, sizeof(temp_file), "/tmp/output_%d.txt", pid);
            FILE *temp_fp = fopen(temp_file, "r");
            if (!temp_fp) {
               send_request(nfd, 500, "Internal Error", "text/html", "<html><body><h1>500 Internal Error</h1></body></html>", 49);
            } else {
               fseek(temp_fp, 0, SEEK_END);
               size_t body_size = ftell(temp_fp);
               fseek(temp_fp, 0, SEEK_SET);
               char *body = malloc(body_size);
               fread(body, 1, body_size, temp_fp);
               send_request(nfd, 200, "OK", "text/html", body, body_size);
               free(body);
               fclose(temp_fp);
            }
         } else {
            send_request(nfd, 500, "Internal Error", "text/html", "<html><body><h1>500 Internal Error</h1></body></html>", 49);
         }
      }
   } else {
      // invalid HTTP Method (Not Implemented)
      send_request(nfd, 501, "Not Implemented", "text/html", "<html><body><h1>501 Not Implemented</h1></body></html>", 47);
   }

   free(line);
   fclose(network);
}

void sigchld_handler(int sig) {
   // avoid zombie processes
   while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main(int argc, char *argv[])
{
   if (argc != 2) {
      fprintf(stderr, "Usage: %s <port>\n", argv[0]);
      exit(1);
   }

   // requirement 1: take one command-line argument specifying 
   // the port on which to listen for connections
   int port = atoi(argv[1]);

   // requirement 2: uses signal handler to clean up child processses
   signal(SIGCHLD, sigchld_handler);

   int fd = create_service(port);

   if (fd == -1)
   {
      perror("create_service");
      exit(1);
   }

   printf("Listening on port: %d\n", port);

   while (1) {
      // ese accept to accept a new client connection
      int client_fd = accept(fd, NULL, NULL);  // accepts the incoming connection
      if (client_fd == -1) {
         perror("accept");
         continue;
      }
      // forks a child process
      pid_t pid = fork();
      if (pid == 0) {
         // while in the child process
         close(fd);  // close the server socket in the child
         handle_request(client_fd);
         close(client_fd);
         exit(0);
      } else if (pid > 0) {
         // while in the parent process
         close(client_fd);  // close the client socket in the parent
      } else {
         perror("fork");
         close(client_fd);
      }
   }

   close(fd);
   return 0;
}
