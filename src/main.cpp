#include <boost/json.hpp>

#include <httpservice/version.hpp>

#include <iostream>

int
main()
{
  boost::json::value body = {{"service", "httpservice"}, {"version", httpservice::get_version()}};
  std::cout << boost::json::serialize(body) << std::endl;
  return 0;
}
