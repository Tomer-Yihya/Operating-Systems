#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>


void signal_handler(int sig_num) {

    struct sigaction sa;
    memset(&sa, 0, sizeof (sa)); 

    if(sig_num == 0) {
        signal(SIGINT, SIG_IGN);
    }

    if(sig_num == 1) {
        signal(SIGCHLD, SIG_IGN);
    }
}

int prepare(void) {
    signal_handler(1);
    signal_handler(0);
    return 0;
}

void restart_signals(void) {
    signal(SIGINT, SIG_DFL);
}

// single process in background
int run_in_background(char** arguments, int ampersand_index) {

    arguments[ampersand_index] = NULL;

    int pid_status = fork();
    if (pid_status < 0) {
        fprintf(stderr, "%s", "Error in child\n");
        return 0;
    }
    if (pid_status == 0) {
        signal(SIGINT, SIG_IGN);
        int status = execvp(arguments[0], arguments);
        if (status == -1) {
            fprintf(stderr, "%s", "Error in child\n");
            exit(1);
        }
        return 1;

    } else {
        return 1;
    }
}

// pipe
int run_with_child(char** arguments, int symbol) {

    char** second_command = arguments + symbol + 1;
    arguments[symbol] = NULL;

    // pipe
    int file_args[2];
    if (pipe(file_args) == -1) {
        fprintf(stderr, "%s", "Error in pipe\n");
        return 0;
    }
    int file_reader = file_args[0];
    int file_writer = file_args[1];

    int pid_first = fork();
    if (pid_first < 0) {
        fprintf(stderr, "%s", "Error in child\n");
        return 0;
    }
    // first child
    if (pid_first == 0) {
        restart_signals();
        while((dup2(file_writer, 1) == -1) && (errno == EINTR)){}
        close(file_writer);
        close(file_reader);
        int status = execvp(arguments[0], arguments);
        if (status == -1) {
            fprintf(stderr, "%s", "Error in child\n");
            exit(1);
        }
        return 0;

    } else { 
        close(file_writer);
        int pid_second = fork();
        if (pid_second < 0) {
            fprintf(stderr, "%s", "Error in child\n");
            return 0;
        }
        // second child
        if (pid_second == 0) {
            restart_signals();
            while((dup2(file_reader, 0) == -1) && (errno == EINTR)){}
            close(file_reader);
            int status2 = execvp(second_command[0], second_command);
            if (status2 == -1) {
                fprintf(stderr, "%s", "Error in child\n");
                exit(1);
            }
            return 0;

        } else {
            close(file_reader);
            if (waitpid(pid_first, NULL, 0) != -1 && errno != ECHILD && errno != EINTR) {
                fprintf(stderr, "%s", "Error in child\n");
                return 0;
            }
            if (waitpid(pid_second, NULL, 0) != -1 && errno != ECHILD && errno != EINTR) {
                fprintf(stderr, "%s", "Error in child\n");
                return 0;
            }
            return 1;
        }
    }
}

int run_with_redirection(char** arguments, int symbol) {

    char* output_file = arguments[symbol + 1];
    arguments[symbol] = NULL;
    int pid_status = fork();

    if (pid_status < 0) {
        fprintf(stderr, "%s", "Error in child\n");
        return 0;
    }
    if (pid_status == 0) {
        restart_signals();
        int fd = open(output_file, O_WRONLY | O_CREAT, 0777);
        close(1); 
        dup2(fd, 1);  
        int status = execvp(arguments[0], arguments);
        if (status == -1) {
            fprintf(stderr, "%s", "Error in child\n");
            return 0;
        }
        close(fd);
        return 1;

    } else {
        if (waitpid(pid_status, NULL, 0) != -1 && errno != ECHILD && errno != EINTR) {
            fprintf(stderr, "%s", "Error in child\n");
            return 0;
        }
        return 1;
    }

}

int process_arglist(int count, char** arguments) {

    int return_value = 0;
    int special_char_index = -1;
	for (int i = 0; i < count; i++) {
        if (strcmp(arguments[i], "&") == 0) {
            special_char_index = i;
			break;
        }
        if (strcmp(arguments[i], "|") == 0) {
            special_char_index = i;
			break;
        }
        if (strcmp(arguments[i], ">") == 0) {
            special_char_index = i;
			break;
        }
    }
    // symbols "&", "|", ">" not found
	if (special_char_index == -1) {
		int pid_status = fork();
		if (pid_status < 0) {
			fprintf(stderr, "%s", "Error in child\n");
			return_value = 0;
		}
		if (pid_status == 0) {
			restart_signals();
			int status = execvp(arguments[0], arguments);
			if (status == -1) {
				fprintf(stderr, "%s", "Error in child\n");
				exit(1);
			}
			return_value = 1;

		} else {
            if (waitpid(pid_status, NULL, 0) != -1 && errno != ECHILD && errno != EINTR) {
                fprintf(stderr, "%s", "Error in child\n");
                return 0;
            }
			return_value = 1;
		}
	} else {
		// pipe
        if(strcmp(arguments[special_char_index], "|") == 0) {
            return_value = run_with_child(arguments, special_char_index);
        }
        // run in background
        else if(strcmp(arguments[special_char_index], "&") == 0) {
            return_value = run_in_background(arguments, special_char_index);
        }
        // redirect
        else {
            return_value = run_with_redirection(arguments, special_char_index);
        }
    } 
    return return_value;
}



int finalize(void) {
    return 0;
}