/*
    Project Name : Nabula OS
    Module       : Command Line Interface (CLI)
    Version      : 0.1

    Purpose:
    This program provides a command-based interface
    where users can enter commands such as help, info,
    date, echo, uptime, clear, and exit.

    Note:
    This is currently a hosted C application running
    on Windows, not yet a bootable operating system.
*/

#include <stdio.h>
// Standard Input/Output Library
// Provides printf(), fgets(), getchar(), fflush(), stdin and stdout.
#include <string.h>
// String Handling Library
// Provides strlen(), strcmp(), strchr(), strcspn() and memmove().
#include <stdlib.h>
// Standard Library
// Provides general-purpose utilities, including system().
// Used here to execute the Windows "cls" command.
// Note: system() is platform-dependent.

#include <time.h>
// Time Library
// Provides time_t, time(), ctime() and difftime().
// Used to display the current date/time and calculate CLI uptime.

#include <ctype.h>
// Character Handling Library
// Provides isspace() and other character-checking functions.
// Used to identify spaces, tabs and other whitespace characters.

#define VERSION "0.1"
// Macro Definition
// Defines the version string in one place.
// VERSION can be reused wherever the program displays its version.

#define COMMAND_SIZE 200
// Macro Definition
// Sets the size of the input character array to 200 bytes.
// This provides space for the command entered by the user,
// including the terminating null character.

/*
    Function: showBanner()
    Purpose : Displays the startup banner of Nabula OS.
    void    : The function does not return a value.
    void parameter list means it takes no arguments.
*/
void showBanner(void) {
    printf("\n");
    printf("  ==========================================\n");
    printf("       N A B U L A   O S\n");
    printf("       Command Interface  v%s\n", VERSION);
    printf("  ==========================================\n");
    printf("  Type 'help' to explore available commands.\n");
}

/*
    Function: showHelp()
    Purpose : Displays all supported commands and
              a short description of each command.
*/
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
    printf("  uptime        Show CLI session uptime\n");
}

/*
    Function: showInfo()
    Purpose : Displays basic information about the project.
    Note    : These are program details, not live hardware
              or actual operating-system kernel information.
*/
void showInfo(void) {
    printf("\n--- SYSTEM INFORMATION ---\n");
    printf("Product       : Nabula OS CLI\n");
    printf("Version       : %s\n", VERSION);
    printf("Interface     : Command Line\n");
    printf("Status        : Running...\n");
    printf("Runtime       : Hosted C application\n");
    printf("Host platform : Windows\n");
}

/*
    Function: showVersion()
    Purpose : Displays the current program version.
*/
void showVersion(void) {
    printf("Nabula OS CLI version %s\n", VERSION);
}

/*
    Function: showAbout()
    Purpose : Explains the purpose and design principles
              of the Nabula OS project.
*/
void showAbout(void) {
    printf("\n--- ABOUT NABULA OS ---\n");
    printf("Nabula OS provides a direct, command-oriented\n");
    printf("computing interface designed for simplicity,\n");
    printf("clarity, and lightweight interaction.\n");

    printf("\nPRODUCT PRINCIPLES\n");
    printf("  - Simple command interaction\n");
    printf("  - Clear system feedback\n");
    printf("  - Lightweight interface\n");
    printf("  - Consistent user experience\n");
}

/*
    Function: showDate()
    Purpose : Displays the current system date and time.

    time(NULL):
    Retrieves the current calendar time.

    ctime():
    Converts the time value into a readable date/time string.
*/
void showDate(void) {
    time_t now = time(NULL);
    char *dateText = ctime(&now);

    // Check whether the date/time string was obtained.
    if (dateText == NULL) {
        printf("Error: Date and time unavailable.\n");
        return;
    }

    printf("Current date and time: %s", dateText);
}

/*
    Function: showUptime()
    Purpose : Calculates how long the CLI has been running.

    startTime:
    The time recorded when main() starts the session.

    difftime():
    Calculates the difference between two time values in seconds.
*/
void showUptime(time_t startTime) {
    time_t now = time(NULL);

    // Calculate elapsed time since the CLI started.
    double elapsed = difftime(now, startTime);

    long totalSeconds;
    long hours;
    long minutes;
    long seconds;

    // Check for an invalid current time or negative elapsed time.
    if (now == (time_t)-1 || elapsed < 0) {
        printf("Error: Session time unavailable.\n");
        return;
    }

    // Convert elapsed seconds into hours, minutes and seconds.
    totalSeconds = (long)elapsed;

    hours = totalSeconds / 3600;
    minutes = (totalSeconds % 3600) / 60;
    seconds = totalSeconds % 60;

    printf("CLI session uptime: %ldh %ldm %lds\n",
           hours, minutes, seconds);
}

/*
    Function: clearScreen()
    Purpose : Attempts to clear the terminal screen.

    system("cls"):
    Runs the Windows command used to clear the console.

    Note:
    This function is Windows-specific and uses system().
*/
void clearScreen(void) {
    if (system("cls") != 0) {
        printf("Unable to clear the terminal.\n");
    }
}

/*
    Function: trimWhitespace()
    Purpose : Removes whitespace from the beginning
              and end of a string.

    Example:
       Input  : "   help   "
       Output : "help"

    isspace():
    Checks for spaces, tabs, newlines and other whitespace.

    memmove():
    Safely moves overlapping regions of memory.
    It is used to shift the remaining string to the beginning.
*/
void trimWhitespace(char *text) {
    size_t length;
    size_t start = 0;

    // Find the first character that is not whitespace.
    while (isspace((unsigned char)text[start])) {
        start++;
    }

    // Remove leading whitespace by shifting the string left.
    if (start > 0) {
        memmove(text, text + start, strlen(text + start) + 1);
    }

    // Calculate the length after removing leading whitespace.
    length = strlen(text);

    // Remove trailing whitespace by inserting '\0'.
    while (length > 0 &&
           isspace((unsigned char)text[length - 1])) {
        text[length - 1] = '\0';
        length--;
    }
}

/*
    Function: main()
    Purpose : Entry point of the C program.

    Main responsibilities:
    1. Initialize the command buffer and session timer.
    2. Display the startup banner.
    3. Read commands continuously.
    4. Parse commands and their arguments.
    5. Execute the matching function.
    6. Exit when requested or when input closes.
*/
int main(void) {

    // Character array used to store the user's input.
    char input[COMMAND_SIZE];

    // Pointer to the arguments following a command.
    char *arguments;

    // Pointer to the first space separating command and arguments.
    char *separator;

    // Record the start time of the CLI session.
    time_t startTime = time(NULL);

    // Display the startup banner once.
    showBanner();

    // Infinite loop: continue accepting commands until exit.
    while (1) {

        // Display the command prompt.
        printf("\nNabula> ");

        // Ensure the prompt appears immediately.
        fflush(stdout);

        // Read a complete line from standard input.
        // fgets() reads at most sizeof(input) - 1 characters.
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\nInput closed. Exiting Nabula CLI.\n");
            break;
        }
        /*
            If the input buffer contains no newline and the
            input stream has not ended, the line may be too long.
            Discard the remaining characters to prevent them
            from being interpreted as another command.
        */
        if (strchr(input, '\n') == NULL && !feof(stdin)) {
            int ch;

            while ((ch = getchar()) != '\n' && ch != EOF) {
                // Discard extra input characters.
            }

            printf("Error: Input is too long. Try again.\n");
            continue;
        }

        // Remove the newline character from the input, if present.
        input[strcspn(input, "\n")] = '\0';

        // Remove leading and trailing whitespace.
        trimWhitespace(input);

        // Ignore an empty command.
        if (input[0] == '\0') {
            continue;
        }

        /*
            Separate the command from its arguments.

            Example:
                Input      : echo Hello World
                Command    : echo
                Arguments  : Hello World
        */
        separator = strchr(input, ' ');
        arguments = NULL;

        if (separator != NULL) {
            // Replace the separating space with the string terminator.
            *separator = '\0';

            // Arguments begin immediately after the separator.
            arguments = separator + 1;

            // Remove extra whitespace around the arguments.
            trimWhitespace(arguments);
        }

        /*
            Command Dispatcher:
            strcmp() compares the entered command with each
            supported command. A result of 0 means they match.
        */

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
            // Displays a predefined username.
            // This is not the actual Windows account lookup.
            printf("nabula-user\n");
        }
        else if (strcmp(input, "uptime") == 0) {
            // Display how long this CLI session has been running.
            showUptime(startTime);
        }
        else if (strcmp(input, "echo") == 0) {

            // Check whether the user supplied any text.
            if (arguments != NULL && arguments[0] != '\0') {
                printf("%s\n", arguments);
            }
            else {
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
            // Handle commands that are not recognized.
            printf("Unknown command: %s\n", input);
            printf("Type 'help' to view available commands.\n");
        }
    }

    // Display a closing message when the loop ends.
    printf("Nabula CLI closed.\n");

    // Return 0 to indicate successful program termination.
    return 0;
}