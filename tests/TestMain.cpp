#include "TestHarness.h"
#include <cstring>

void RunMovementTests();
void RunPolicyTests();
void RunRepositoryTests();
void RunConfigTests();
void RunSequenceTests();

int main(int argc, char** argv)
{
    try
    {
        bool ran = false;
        auto run = [&](char const* name, void (*suite)())
        {
            if (argc == 1 || std::strcmp(argv[1], name) == 0)
            {
                suite();
                std::cout << name << " passed\n";
                ran = true;
            }
        };
        run("movement", RunMovementTests);
        run("policy", RunPolicyTests);
        run("repository", RunRepositoryTests);
        run("config", RunConfigTests);
        run("sequences", RunSequenceTests);
        if (!ran)
            throw std::runtime_error("unknown test suite");
        std::cout << anticheat::test::checks << " checks passed\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
