#include "config.h"
#include "libglnx.h"
#include "rpmostree-kargs-process.h"

static gboolean
check_string_existance (OstreeKernelArgs *karg,
                        const char *string_to_find)
{
  g_autofree gchar* string_with_spaces = _ostree_kernel_args_to_string (karg);
  g_auto(GStrv) string_list = g_strsplit (string_with_spaces, " ", -1);
  return g_strv_contains ((const char* const*) string_list, string_to_find);
}

static void
test_kargs_delete (void)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;
  int key_index = 0;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *karg = _ostree_kernel_args_new ();

  _ostree_kernel_args_append (karg, "single_key=test");
  _ostree_kernel_args_append (karg, "test=firstval");
  _ostree_kernel_args_append (karg, "test=secondval");

  /* Delete a non-existant key should fail */
  ret = _ostree_kernel_args_delete (karg, "non_existant_key", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with multiple values when only specifying key should fail */
  ret = _ostree_kernel_args_delete (karg, "test", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with a non existant value should fail */
  ret = _ostree_kernel_args_delete (karg, "test=non_existant_value", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Delete a key with only one value should succeed by only specifying key */
  ret = _ostree_kernel_args_delete (karg, "single_key", &error);
  g_assert_no_error (error);
  g_assert (ret);
  /* verify the value array is properly updated */
  GPtrArray *kargs_array = _ostree_kernel_arg_get_key_array (karg);
  g_assert (!_ostree_ptr_array_find (kargs_array, "single_key", &key_index));
  g_assert (!check_string_existance (karg, "single_key"));

  /* Delete a key/value pair by specifying correct key=value should succeed */
  ret = _ostree_kernel_args_delete (karg, "test=secondval", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (!check_string_existance (karg, "test=secondval"));

}

static void
test_kargs_replace (void)
{
  g_autoptr(GError) error = NULL;
  gboolean ret;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *karg = _ostree_kernel_args_new ();

  _ostree_kernel_args_append (karg, "single_key");
  _ostree_kernel_args_append (karg, "test=firstval");
  _ostree_kernel_args_append (karg, "test=secondval");

  /* Replace when the input key is non-existant should fail */
  ret = _ostree_kernel_args_new_replace (karg, "nonexistantkey", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace non-existant value with input key=nonexistantvalue=newvalue should fail */
  ret = _ostree_kernel_args_new_replace (karg, "single_key=nonexistantval=newval", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace with input key=value will fail for a key with multiple values */
  ret = _ostree_kernel_args_new_replace (karg, "test=newval", &error);
  g_assert (!ret);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  /* Replace with input key=value for a key with single value should succeed
   * Also note, we also allow ''(empty string) valid to be a value
   */
  ret = _ostree_kernel_args_new_replace (karg, "single_key=newvalue", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (check_string_existance (karg, "single_key=newvalue"));

  /* Replace with input key=value=newvalue if key and value both
   * exist, the action should succeed
   */
  ret = _ostree_kernel_args_new_replace (karg, "test=firstval=newval", &error);
  g_assert_no_error (error);
  g_assert (ret);
  g_assert (check_string_existance (karg, "test=newval"));
}


/* In this function, we want to verify that _ostree_kernel_args_append
 * and _ostree_kernel_args_to_string is correct. After that
 * we will use these two functions(append and tostring) in other tests: delete and replace
 */
static void
test_kargs_append (void)

{
  /* Note, here we assume the _ostree_ptr_array_find has
   * a working functionality, as it shares a similar logic
   * as the GLib function, and easy to tell the correctness of it
   */
  int test_num = 0;
  __attribute__((cleanup(_ostree_kernel_args_cleanup))) OstreeKernelArgs *append_arg = _ostree_kernel_args_new ();
  /* Some valid cases (key=value) pair */
  _ostree_kernel_args_append (append_arg, "test=valid");
  _ostree_kernel_args_append (append_arg, "test=secondvalid");
  /* Add some 'invalid cases' */
  _ostree_kernel_args_append (append_arg, "second_test");

  /* We loops through the kargs inside table to verify
   * the functionality of append because at this stage
   * we have yet to find the conversion kargs to string fully "functional"
   */
  GHashTable *kargs_table = _ostree_kernel_arg_get_kargs_table (append_arg);
  GLNX_HASH_TABLE_FOREACH_KV (kargs_table, const char*, key, GPtrArray*, value_array)
    {
      if ( g_strcmp0 (key, "test") == 0)
        {
          g_assert (_ostree_ptr_array_find (value_array, "valid", &test_num));
          g_assert (_ostree_ptr_array_find (value_array, "secondvalid", &test_num));
        }
      else
        g_assert (_ostree_ptr_array_find (value_array, "", &test_num));
    }
  /* verify the value array is properly updated */
  GPtrArray *kargs_array = _ostree_kernel_arg_get_key_array (append_arg);
  g_assert (_ostree_ptr_array_find (kargs_array, "test", &test_num));
  g_assert (_ostree_ptr_array_find (kargs_array, "second_test", &test_num));

  /* Up till this point, we verified that the above was all correct, we then
   * check _ostree_kernel_args_to_string has the right result
   */
  g_autofree gchar* kargs_str = _ostree_kernel_args_to_string (append_arg);
  g_auto(GStrv) kargs_list = g_strsplit(kargs_str, " ", -1);
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test=valid"));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "second_test"));
  g_assert (g_strv_contains ((const char* const *)kargs_list, "test=secondvalid"));
  g_assert_cmpint (3, ==, g_strv_length (kargs_list));

}

int
main (int argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/kargs/kargs_append", test_kargs_append);
  g_test_add_func ("/kargs/kargs_delete", test_kargs_delete);
  g_test_add_func ("/kargs/kargs_replace", test_kargs_replace);
  return g_test_run ();
}
