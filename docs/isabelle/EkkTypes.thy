(*
 * EkkTypes.thy -- Shared AutoCorres import for ekkor-agentic-experiment.
 *
 * Installs the unified verification C file and runs AutoCorres.
 * All subsequent theories import this one to get the lifted Isabelle
 * definitions.
 *
 * C file processed:
 *   ekk_verif.c -- all proof targets (B1/B2/B3) in one translation unit
 *                  to avoid duplicate constant declarations from shared headers.
 *
 * Include path: ../../include (relative to this file's session directory).
 * The l4v C parser sees EKK_VERIFICATION via the #define at the top of
 * ekk_verif.c.
 *)

theory EkkTypes
imports
  "AutoCorres.AutoCorres"
begin

(* Tell the C parser where ekk/ headers live *)
new_C_include_dir "../../include"

(* Register external file dependency so Isabelle tracks it *)
external_file "ekk_verif.c"

(* Parse and lift all verification targets in one shot *)
install_C_file "ekk_verif.c"
autocorres "ekk_verif.c"

end
