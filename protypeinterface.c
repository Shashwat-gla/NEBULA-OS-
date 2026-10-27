
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#define VERSION "0.1"
#define COMMAND_SIZE 200

void showBanner(void) {
    printf("\n");
    printf("  ==========================================\n");
    printf("       N A B U L A   O S\n");
    printf("       Command Interface  v%s\n", VERSION);
    printf("  ==========================================\n");
    printf("  Type 'help' to explore available commands.\n");
}

void showHelp(void) {
    printf("\n--- NABULA COMMAND DIRECTORY ---\n");
    printf("  help          Show all commands\n");
    printf("  info          Display system information\n");
    printf("  version       Display product version\n");
    printf("  about         About Nabula OS\n");
    printf("  date          Show current date and time\n");
    printf("  whoami        Show current user\n");
    printf("  echo <text>   Display a message\n");
    printf("  clear         Clear the terminal screen\n");
    printf("  exit          Close the command interface\n");
}

void showInfo(void) {
    printf("\n--- SYSTEM INFORMATION ---\n");
    printf("Product       : Nabula OS CLI\n");
    printf("Version       : %s\n", VERSION);
    printf("Interface     : Command Line\n");
    printf("Status        : Running...\n");
    printf("Runtime       : Hosted C application\n");
    printf("Host platform : Windows)\n");
}

void showVersion(void) {
    printf("Nabula OS CLI version %s\n", VERSION);
}
void showAbout(void) {
    printf("\n--- ABOUT NABULA OS ---\n");
    printf("Nabula OS provides a direct, command-oriented\n");
    printf("computing interface designed for simplicity,\n");
    printf("clarity, and lightweight interaction.\n");
    printf("\n");
    printf("PRODUCT PRINCIPLES\n");
    printf("  - Simple command interaction\n");
    printf("  - Clear system feedback\n");
    printf("  - Lightweight interface\n");
    printf("  - Consistent user experience\n");
}

void showDate(void) {
    time_t now = time(NULL);
    char *dateText = ctime(&now);

    if (dateText == NULL) {
        printf("Error: Date and time unavailable.\n");
        return;
    }

    printf("Current date and time: %s", dateText);
}

void showUptime(time_t startTime) {
    time_t now = time(NULL);
    double elapsed = difftime(now, startTime);
    long totalSeconds;
    long hours;
    long minutes;
    long seconds;

    if (now == (time_t)-1 || elapsed < 0) {
        printf("Error: Session time unavailable.\n");
        return;
    }

    totalSeconds = (long)elapsed;
    hours = totalSeconds / 3600;
    minutes = (totalSeconds % 3600) / 60;
    seconds = totalSeconds % 60;

    printf("CLI session uptime: %ldh %ldm %lds\n",
           hours, minutes, seconds);
}

void clearScreen(void) {
    if (system("cls") != 0) {
        printf("Unable to clear the terminal.\n");
    }
}

void trimWhitespace(char *text) {
    size_t length;
    size_t start = 0;

    while (isspace((unsigned char)text[start])) {
        start++;
    }

    if (start > 0) {
        memmove(text, text + start, strlen(text + start) + 1);
    }

    length = strlen(text);

    while (length > 0 &&
           isspace((unsigned char)text[length - 1])) {
        text[length - 1] = '\0';
        length--;
    }
}

int main(void) {
    char input[COMMAND_SIZE];
    char *arguments;
    char *separator;
    time_t startTime = time(NULL);

    showBanner();

    while (1) {
        printf("\nNabula> ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\nInput closed. Exiting Nabula CLI.\n");
            break;
        }

        if (strchr(input, '\n') == NULL && !feof(stdin)) {
            int ch;

            while ((ch = getchar()) != '\n' && ch != EOF) {
                /* Discard extra input */
            }

            printf("Error: Input is too long. Try again.\n");
            continue;
        }

        input[strcspn(input, "\n")] = '\0';
        trimWhitespace(input);

        if (input[0] == '\0') {
            continue;
        }

        separator = strchr(input, ' ');
        arguments = NULL;

        if (separator != NULL) {
            *separator = '\0';
            arguments = separator + 1;
            trimWhitespace(arguments);
        }

        if (strcmp(input, "help") == 0) {
            showHelp();
        }
        else if (strcmp(input, "info") == 0) {
            showInfo();
        }
        else if (strcmp(input, "version") == 0) {
            showVersion();
        }
        else if (strcmp(input, "about") == 0) {
            showAbout();
        }
        else if (strcmp(input, "date") == 0) {
            showDate();
        }
        else if (strcmp(input, "whoami") == 0) {
            printf("nabula-user\n");
        }
        else if (strcmp(input, "uptime") == 0) {
            showUptime(startTime);
        }
        else if (strcmp(input, "echo") == 0) {
            if (arguments != NULL && arguments[0] != '\0') {
                printf("%s\n", arguments);
            } else {
                printf("Usage: echo <text>\n");
            }
        }
        else if (strcmp(input, "clear") == 0) {
            clearScreen();
        }
        else if (strcmp(input, "exit") == 0) {
            printf("Closing Nabula command interface...\n");
            break;
        }
        else {
            printf("Unknown command: %s\n", input);
            printf("Type 'help' to view available commands.\n");
        }
    }

    printf("Nabula CLI closed.\n");
    return 0;
}