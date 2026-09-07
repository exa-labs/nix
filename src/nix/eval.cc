#include "nix/cmd/command-installable-value.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/main/common-args.hh"
#include "nix/main/shared.hh"
#include "nix/store/store-api.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/print.hh"
#include "nix/expr/value-to-json.hh"

#include <nlohmann/json.hpp>
#include <sstream>

namespace nix {

struct CmdEval : MixJSON, InstallableValueCommand, MixReadOnlyOption
{
    bool raw = false;
    std::optional<std::string> apply;
    std::optional<std::filesystem::path> writeTo;

    CmdEval()
        : InstallableValueCommand()
    {
        addFlag({
            .longName = "raw",
            .description = "Print strings without quotes or escaping.",
            .handler = {&raw, true},
        });

        addFlag({
            .longName = "apply",
            .description = "Apply the function *expr* to each argument.",
            .labels = {"expr"},
            .handler = {&apply},
        });

        addFlag({
            .longName = "write-to",
            .description = "Write a string or attrset of strings to *path*.",
            .labels = {"path"},
            .handler = {&writeTo},
        });
    }

    std::string description() override
    {
        return "evaluate a Nix expression";
    }

    std::string doc() override
    {
        return
#include "eval.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    void run(ref<Store> store, ref<InstallableValue> installable) override
    {
        if (raw && json)
            throw UsageError("--raw and --json are mutually exclusive");

        auto state = getEvalState();

        /* Fast path: for a plain flake attribute eval (no --apply, no
           --write-to), try to serve a string value straight from the
           eval cache. This skips evaluating the flake (and in particular
           derivationStrict) entirely when the inputs haven't changed.
           cachedGetStringWithContext() only returns on a genuine cache
           hit, so we never use a value that would have required
           evaluation. */
        std::pair<Value *, PosIdx> valueAndPos;
        auto flakeInstallable = dynamic_cast<InstallableFlake *>(&*installable);
        if (flakeInstallable && !apply && !writeTo) {
            /* Resolve the cursor once and reuse it for the slow path;
               InstallableFlake::toValue() is exactly
               getCursor()->forceValue(). */
            auto cursor = flakeInstallable->getCursor(*state, AutoCall::No);
            if (auto cached = cursor->cachedGetStringWithContext()) {
                auto & [s, ctx] = *cached;
                if (raw) {
                    logger->stop();
                    writeFull(getStandardOutput(), s);
                } else if (json) {
                    printJSON(nlohmann::json(s));
                } else {
                    std::ostringstream out;
                    printLiteralString(out, s);
                    logger->cout("%s", out.str());
                }
                state->ensureLazyPathsCopied(ctx);
                return;
            }
            valueAndPos = {&cursor->forceValue(), noPos};
        } else
            valueAndPos = installable->toValue(*state, AutoCall::No);

        auto [v, pos] = valueAndPos;
        NixStringContext context;

        if (apply) {
            auto vApply = state->allocValue();
            state->eval(state->parseExprFromString(*apply, state->rootPath(".")), *vApply);
            auto vRes = state->allocValue();
            state->callFunction(*vApply, *v, *vRes, noPos);
            v = vRes;
        }

        if (writeTo) {
            logger->stop();

            if (pathExists(*writeTo))
                throw Error("path '%s' already exists", writeTo->string());

            [&](this const auto & recurse, Value & v, const PosIdx pos, const std::filesystem::path & path) -> void {
                state->forceValue(v, pos);
                if (v.type() == nString) {
                    copyContext(v, context);
                    writeFile(path, v.string_view());
                } else if (v.type() == nAttrs) {
                    [[maybe_unused]] bool directoryCreated = std::filesystem::create_directory(path);
                    // Directory should not already exist
                    assert(directoryCreated);
                    for (auto & attr : *v.attrs()) {
                        std::string_view name = state->symbols[attr.name];
                        try {
                            if (name == "." || name == "..")
                                throw Error("invalid file name '%s'", name);
                            recurse(*attr.value, attr.pos, path / name);
                        } catch (Error & e) {
                            e.addTrace(
                                state->positions[attr.pos], HintFmt("while evaluating the attribute '%s'", name));
                            throw;
                        }
                    }
                } else
                    state->error<TypeError>("value at '%s' is not a string or an attribute set", state->positions[pos])
                        .debugThrow();
            }(*v, pos, *writeTo);
        }

        else if (raw) {
            logger->stop();
            auto string = state->coerceToString(noPos, *v, context, "while generating the eval command output");
            writeFull(getStandardOutput(), *string);
        }

        else if (json) {
            printJSON(printValueAsJSON(*state, true, *v, pos, context, false));
        }

        else {
            ValuePrinter printer(*state, *v, PrintOptions{.force = true, .derivationPaths = true}, &context);
            logger->cout("%s", printer);
        }

        state->ensureLazyPathsCopied(context);
    }
};

static auto rCmdEval = registerCommand<CmdEval>("eval");

} // namespace nix
