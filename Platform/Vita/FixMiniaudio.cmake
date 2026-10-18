# miniaudio 0.11.23 writes to memory it has freed.
#
# Its resource manager frees what a sound was loaded into on a thread of its own when the
# sound is let go of while its file is still being decoded. Two of the three jobs that do
# this count the job as done in the object after the object is gone:
#
#   ma_job_process__resource_manager__free_data_buffer_node()  frees the node, then
#       increments pDataBufferNode->executionPointer
#   ma_job_process__resource_manager__free_data_buffer()       wakes the thread that waits
#       to free the buffer, then increments pDataBuffer->executionPointer
#
# The third, ..._free_data_stream(), has the same line commented out. Whoever was given
# the memory in the meantime finds one of its bytes changed. A game lets go of sounds that
# are still loading all the time: a unit is told to be quiet, a loop changes its file.
#
# vita_fix_miniaudio(<miniaudio.h of the release> <corrected copy to write>)
#
# The release is not touched; the copy is what gets compiled. With another release this
# fails, on purpose: whether the lines are still there, and still wrong, has to be looked
# at again.
function(vita_fix_miniaudio original corrected)
    file(READ "${original}" header)

    foreach(job IN ITEMS "freeDataBufferNode|pDataBufferNode" "freeDataBuffer|pDataBuffer")
        string(REPLACE "|" ";" job "${job}")
        list(GET job 0 name)
        list(GET job 1 object)

        # The last thing the job does before the line in question, found once in the file.
        set(anchor "ma_fence_release(pJob->data.resourceManager.${name}.pDoneFence);")
        set(late_write "ma_atomic_fetch_add_32(&${object}->executionPointer, 1);")

        string(FIND "${header}" "${anchor}" first)
        string(FIND "${header}" "${anchor}" last REVERSE)
        if(first EQUAL -1 OR NOT first EQUAL last)
            message(FATAL_ERROR "FixMiniaudio: '${anchor}' is not in ${original} exactly once. Is this still miniaudio 0.11.23?")
        endif()

        string(SUBSTRING "${header}" 0 ${first} head)
        string(SUBSTRING "${header}" ${first} -1 tail)
        string(FIND "${tail}" "${late_write}" offset)
        if(offset EQUAL -1 OR offset GREATER 200)
            message(FATAL_ERROR "FixMiniaudio: '${late_write}' does not follow '${anchor}' in ${original}. Is this still miniaudio 0.11.23?")
        endif()

        string(LENGTH "${late_write}" length)
        math(EXPR rest "${offset} + ${length}")
        string(SUBSTRING "${tail}" 0 ${offset} before)
        string(SUBSTRING "${tail}" ${rest} -1 after)
        set(header "${head}${before}/* GeneralsX: ${object} is gone by now, see Platform/Vita/FixMiniaudio.cmake. */${after}")
    endforeach()

    # Written only when it differs, so that configuring again does not rebuild what includes it.
    set(existing "")
    if(EXISTS "${corrected}")
        file(READ "${corrected}" existing)
    endif()
    if(NOT existing STREQUAL header)
        file(WRITE "${corrected}" "${header}")
    endif()
endfunction()
