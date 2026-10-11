// cases.h: the cases of the checks on the virtual CHOMPI (ui.cpp, and through timing.h midi.cpp,
// sync.cpp and midiout.cpp), each on a freshly booted device: a process forked for it, as the
// firmware's globals allow one power cycle per process. RunAllCases() runs them side by side,
// CASE_JOBS at a time (the cores by default), and prints what each printed in the order they
// were declared. A case that takes the card another one kept (KeepCard(), TakeCard()) runs
// after it, on that chain's card: Chain() names them. A case still running after CASE_TIMEOUT
// seconds (60) is stopped and fails by name. CASES="a b" runs only those, with the cases
// before them in their chains; a name no case has fails the run.
#pragma once
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
#include "twin.h"

struct Case
{
    std::string name;
    std::function<void()> run;
};
static std::vector<Case> cases;

// The cases that pass the card on, in the order they run
static std::vector<std::vector<std::string>> chains;
__attribute__((unused)) static void Chain(std::initializer_list<const char*> names)
{
    chains.emplace_back(names.begin(), names.end());
}

// The run's scratch folder, removed when the run ends
static std::string run_dir;
static pid_t run_dir_owner = 0;
static void RemoveRunDir()
{
    if (getpid() == run_dir_owner)
        if (system(("rm -rf '" + run_dir + "'").c_str()) != 0)
            fprintf(stderr, "couldn't remove %s\n", run_dir.c_str());
}
static const std::string& RunDir()
{
    if (run_dir.empty())
    {
        const char* tmp = getenv("TMPDIR");
        std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/frizz-cases-XXXXXX";
        if (!mkdtemp(&dir[0]))
        {
            perror("cases.h: mkdtemp");
            exit(1);
        }
        run_dir = dir;
        run_dir_owner = getpid();
        atexit(RemoveRunDir);
    }
    return run_dir;
}

// The card across power cycles: a case can leave its card for the next of its chain
static std::string case_name, card_file;
__attribute__((unused)) static void KeepCard()
{
    std::ofstream f(card_file, std::ios::binary);
    for (auto& c : twin::CardFiles())
        f << c.first << '\n' << c.second.size() << '\n' << c.second;
}
__attribute__((unused)) static void TakeCard()
{
    std::ifstream f(card_file, std::ios::binary);
    if (!f)
    {
        Check(false, (case_name + ": takes a card no case before it in its chain kept (Chain())").c_str());
        return;
    }
    std::string path, len;
    while (std::getline(f, path) && std::getline(f, len))
    {
        std::string body(std::stoul(len), '\0');
        f.read(&body[0], body.size());
        twin::CardFiles()[path] = body;
    }
}

/** Runs the cases, adding their failures and known faults to check.h's counts; boots says
 *  which start on a booted device (all by default; the others boot by themselves, as a script
 *  does) */
__attribute__((unused)) static void RunAllCases(const std::function<bool(const Case&)>& boots = nullptr)
{
    const size_t n = cases.size();
    RunDir();

    // the groups that run one case after the other: a chain, or a case on its own
    std::vector<int> group(n, -1);
    std::vector<std::vector<size_t>> groups;
    for (const auto& chain : chains)
    {
        std::vector<size_t> members;
        size_t from = 0;
        for (const std::string& name : chain)
        {
            size_t i = from;
            while (i < n && cases[i].name != name)
                i++;
            if (i == n || group[i] >= 0)
            {
                Check(false, ("Chain(): no case " + name + " after the one before it, outside other chains").c_str());
                continue;
            }
            group[i] = int(groups.size());
            members.push_back(i);
            from = i + 1;
        }
        groups.push_back(members);
    }
    for (size_t i = 0; i < n; i++)
        if (group[i] < 0)
        {
            group[i] = int(groups.size());
            groups.push_back({i});
        }

    // CASES: those, and the cases before them in their chains
    std::vector<bool> chosen(n, true);
    if (const char* only = getenv("CASES"))
    {
        chosen.assign(n, false);
        std::istringstream words(only);
        std::string word;
        int asked = 0;
        while (words >> word)
        {
            asked++;
            bool found = false;
            for (size_t i = 0; i < n; i++)
                if (cases[i].name == word)
                {
                    found = true;
                    for (size_t j : groups[group[i]])
                    {
                        chosen[j] = true;
                        if (j == i)
                            break;
                    }
                }
            if (!found)
                Check(false, ("CASES: no case " + word).c_str());
        }
        if (!asked)
            Check(false, "CASES: set, but names no case");
    }
    std::vector<std::vector<size_t>> todo; // the groups' chosen cases, in the order they start
    std::map<size_t, size_t> first;        // by their first case
    for (auto& g : groups)
    {
        std::vector<size_t> t;
        for (size_t i : g)
            if (chosen[i])
                t.push_back(i);
        if (!t.empty())
            first[t[0]] = todo.size(), todo.push_back(t);
    }
    std::vector<size_t> order; // todo's indices by their first case
    for (auto& f : first)
        order.push_back(f.second);

    const char* jobs_env = getenv("CASE_JOBS");
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    const size_t jobs = std::max(1L, jobs_env ? atol(jobs_env) : cores);
    const char* timeout_env = getenv("CASE_TIMEOUT");
    const unsigned timeout = timeout_env ? unsigned(atoi(timeout_env)) : 60;

    // what each case's process hands back
    struct Result
    {
        int failures, known, done;
    };
    Result* result = static_cast<Result*>(mmap(nullptr, sizeof(Result) * (n + 1), PROT_READ | PROT_WRITE,
                                               MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (result == MAP_FAILED)
    {
        perror("cases.h: mmap");
        exit(1);
    }
    memset(result, 0, sizeof(Result) * (n + 1));
    auto out_path = [](size_t i) { return run_dir + "/" + std::to_string(i) + ".out"; };

    std::map<pid_t, size_t> running; // by the todo group it's of
    std::vector<size_t> next(todo.size(), 0);
    std::vector<int> status(n, 0);
    std::vector<bool> ended(n, false);
    std::function<void(size_t)> start = [&](size_t t) {
        const size_t i = todo[t][next[t]++];
        fflush(stdout);
        fflush(stderr);
        const pid_t pid = fork();
        if (pid == 0)
        {
            const int fd = open(out_path(i).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
            static char line[BUFSIZ]; // line by line, so what a case that hangs printed still shows
            setvbuf(stdout, line, _IOLBF, sizeof(line));
            alarm(timeout);
            failures = known = 0; // the run's own stay with the run
            case_name = cases[i].name;
            card_file = run_dir + "/card-" + std::to_string(t);
            if (!boots || boots(cases[i]))
                twin::Boot();
            cases[i].run();
            fflush(stdout);
            result[i] = {failures, known, 1};
            _exit(0);
        }
        if (pid < 0)
        {
            perror("cases.h: fork");
            ended[i] = true;
            if (next[t] < todo[t].size())
                start(t);
            return;
        }
        running[pid] = t;
    };

    size_t started = 0, printed = 0;
    while (true)
    {
        while (running.size() < jobs && started < order.size())
            start(order[started++]);
        // what has ended, in the cases' order
        while (printed < n && (!chosen[printed] || ended[printed]))
        {
            const size_t i = printed++;
            if (!chosen[i])
                continue;
            std::ifstream out(out_path(i), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
            fwrite(text.data(), 1, text.size(), stdout);
            const int st = status[i];
            if (WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM)
                printf("FAIL  %s: still running after %u s (CASE_TIMEOUT), stopped\n", cases[i].name.c_str(), timeout);
            else if (WIFSIGNALED(st))
                printf("FAIL  %s: crashed (%s)\n", cases[i].name.c_str(), strsignal(WTERMSIG(st)));
            else if (!result[i].done)
                printf("FAIL  %s: didn't finish\n", cases[i].name.c_str());
            if (!result[i].done)
                failures++;
            failures += result[i].failures;
            known += result[i].known;
        }
        if (running.empty() && started == order.size())
            break;
        int st = 0;
        const pid_t pid = waitpid(-1, &st, 0);
        if (pid < 0)
            break;
        auto it = running.find(pid);
        if (it == running.end())
            continue;
        const size_t t = it->second;
        running.erase(it);
        const size_t i = todo[t][next[t] - 1];
        status[i] = st;
        ended[i] = true;
        if (next[t] < todo[t].size())
            start(t);
    }
    fflush(stdout);
    munmap(result, sizeof(Result) * (n + 1));
}
