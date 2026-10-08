include("${CMAKE_CURRENT_LIST_DIR}/ReferenceIdentity.cmake")
modern_leveldb_reference_identity(
  "${REFERENCE_SOURCE}" "${REFERENCE_OVERRIDE}" revision identity dirty
)
file(READ "${POLICY_BASE}" policy)
string(JSON policy SET "${policy}" reference_revision "\"${revision}\"")
string(JSON policy SET "${policy}" reference_source "\"${identity}\"")
string(JSON policy SET "${policy}" reference_dirty "\"${dirty}\"")
file(WRITE "${POLICY_OUTPUT}.tmp" "${policy}\n")
file(RENAME "${POLICY_OUTPUT}.tmp" "${POLICY_OUTPUT}")
