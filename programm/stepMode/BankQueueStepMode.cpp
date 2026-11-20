#include "../BankQueueConsole.h"

int main(int argc, char** argv) {
    return bank_queue::runConsole(argc, argv, bank_queue::Mode::Step);
}
