// webtwin.cpp: the twin for the browser (../twin/build.sh wasm, what web/serve.sh serves, #441),
// where Emscripten is (emsdk in ~/opt/emsdk, or em++ on the PATH): it builds from the working
// tree, DaisySP for the browser with a stamp of its sources and em++ as the host's has, and the
// twin it built runs in node (emsdk's): boots, its main loop running after 3 s, its audio
// finite, its version the working tree's. Without Emscripten it prints SKIP: local only (Actions
// has none). Run by unit.sh webtwin.
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "check.h"

/** Runs cmd in bash, stdout and stderr into out; its exit code (-1 for a signal) */
static int Run(const std::string& cmd, std::string& out)
{
    out.clear();
    std::string quoted = "'";
    for (char c : cmd)
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    FILE* p = popen(("bash -c " + quoted + "' 2>&1").c_str(), "r");
    char buf[512];
    size_t n;
    while (p && (n = fread(buf, 1, sizeof(buf), p)) > 0)
        out.append(buf, n);
    const int status = p ? pclose(p) : -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool Has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

static std::string Read(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main()
{
    char* real = realpath((std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/..").c_str(), nullptr);
    const std::string firmware = real ? real : "?";
    free(real);
    const std::string emsdk = "command -v em++ > /dev/null || source ~/opt/emsdk/emsdk_env.sh > /dev/null 2>&1; ";
    std::string out;
    if (Run(emsdk + "command -v em++", out) != 0)
    {
        printf("SKIP: local only (Emscripten in ~/opt/emsdk): the twin for the browser not built\n");
        return Finish();
    }
    // build.sh's tree is the host twin's too (twin/build): one build.sh at a time there (lib.sh)
    const std::string twin = firmware + "/twin", web = twin + "/web/build";
    int rc = Run(emsdk + "mkdir -p '" + twin + "/build' && flock '" + twin + "/build/build.lock' '" + twin + "/build.sh' wasm",
                 out);
    Check(rc == 0 && Read(web + "/frizz-twin.wasm").size() > 100000 && Read(web + "/twin.stamp").size() == 33,
          "webtwin: build.sh wasm builds the twin for the browser from the working tree");
    if (rc != 0)
        printf("%s\n", out.c_str());
    const std::string stamp = Read(twin + "/build/daisysp-wasm.stamp");
    Check(stamp.size() == 33 && stamp.find_first_not_of("0123456789abcdef\n") == std::string::npos,
          "webtwin: DaisySP for the browser carries the stamp of its sources and em++ (rebuilt when they change)");

    char dir[] = "/tmp/frizz-webtwin-XXXXXX";
    if (!mkdtemp(dir))
    {
        Check(false, "webtwin: a temp folder");
        return Finish();
    }
    const std::string js = std::string(dir) + "/smoke.js";
    std::ofstream(js) << "const FrizzTwin = require(process.argv[2]);\n"
                         "FrizzTwin().then(m => {\n"
                         "  m._twin_boot();\n"
                         "  const most = m._twin_max_blocks();\n"
                         "  for (let n = 0; n < 6000; n += most) m._twin_run(Math.min(most, 6000 - n));\n"
                         "  const at = m._twin_out() / 4, out = m.HEAPF32.subarray(at, at + 24 * 4);\n"
                         "  console.log('version ' + m.UTF8ToString(m._twin_version()) + ' now ' + m._twin_now_ms()\n"
                         "              + ' powered ' + m._twin_powered() + ' main loop ' + m._twin_main_loop_running()\n"
                         "              + ' finite ' + out.every(Number.isFinite));\n"
                         "});\n";
    rc = Run(emsdk + "\"${EMSDK_NODE:-node}\" '" + js + "' '" + web + "/frizz-twin.js'", out);
    printf("      %s", out.c_str());
    std::string head;
    Run("git -C '" + firmware + "' rev-parse --short HEAD", head);
    head = head.substr(0, head.find('\n'));
    Check(rc == 0 && Has(out, "version " + head) && Has(out, " now 3000 powered 1 main loop 1 finite true"),
          "webtwin: it runs in node: boots, its main loop running after 3 s, its audio finite, the working tree's");
    if (system(("rm -rf '" + std::string(dir) + "'").c_str()) != 0)
        printf("could not remove %s\n", dir);
    return Finish();
}
