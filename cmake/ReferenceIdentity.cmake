function(modern_leveldb_reference_identity source override revision_output identity_output dirty_output)
  set(revision "7ee830d02b623e8ffe0b95d59a74db1e58da04c5")
  set(identity "authenticated-pinned-archive")
  set(dirty "not-applicable")
  if(override)
    set(revision "unknown")
    set(identity "external-source-override")
    set(dirty "unknown")
    execute_process(
      COMMAND git -C "${source}" rev-parse --show-toplevel
      RESULT_VARIABLE root_status OUTPUT_VARIABLE git_root
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    file(REAL_PATH "${source}" canonical_source)
    if(root_status EQUAL 0)
      file(REAL_PATH "${git_root}" canonical_git_root)
      if(canonical_source STREQUAL canonical_git_root)
        execute_process(
          COMMAND git -C "${source}" rev-parse HEAD
          RESULT_VARIABLE revision_status OUTPUT_VARIABLE candidate_revision
          OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        )
        execute_process(
          COMMAND git -C "${source}" status --porcelain --untracked-files=normal
          RESULT_VARIABLE changes_status OUTPUT_VARIABLE changes
          OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
        )
        string(LENGTH "${candidate_revision}" revision_length)
        if(revision_status EQUAL 0 AND revision_length EQUAL 40 AND
           candidate_revision MATCHES "^[0-9a-f]+$" AND changes_status EQUAL 0)
          set(revision "${candidate_revision}")
          if(changes STREQUAL "")
            set(identity "git-source-override")
            set(dirty "clean")
          else()
            set(identity "modified-git-source-override")
            set(dirty "modified")
          endif()
        endif()
      endif()
    endif()
  endif()
  set("${revision_output}" "${revision}" PARENT_SCOPE)
  set("${identity_output}" "${identity}" PARENT_SCOPE)
  set("${dirty_output}" "${dirty}" PARENT_SCOPE)
endfunction()
